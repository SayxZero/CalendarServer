#include "workerpool.h"

#include <QDebug>
#include <QThread>

#include <chrono>
#include <climits>
#include <exception>

namespace {

qint64 steadyNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

template <typename T>
void raiseTo(std::atomic<T> &maximum, T value)
{
    T seen = maximum.load();
    while (value > seen && !maximum.compare_exchange_weak(seen, value)) {
    }
}

} // namespace

QString dispatchModeKey(DispatchMode mode)
{
    switch (mode) {
    case DispatchMode::Fair: return QStringLiteral("fair");
    case DispatchMode::Fifo: return QStringLiteral("fifo");
    case DispatchMode::RoundRobin: return QStringLiteral("round_robin");
    case DispatchMode::LeastLoaded: return QStringLiteral("least_loaded");
    }
    return QStringLiteral("fair");
}

DispatchMode dispatchModeFromKey(const QString &key, bool *ok)
{
    const QString name = key.trimmed().toLower();
    const DispatchMode all[] = { DispatchMode::Fair, DispatchMode::Fifo, DispatchMode::RoundRobin,
                                 DispatchMode::LeastLoaded };
    for (DispatchMode mode : all) {
        if (dispatchModeKey(mode) == name) {
            if (ok)
                *ok = true;
            return mode;
        }
    }
    if (ok)
        *ok = false;
    return DispatchMode::Fair;
}

int WorkerPool::defaultThreads()
{
    return qMax(2, QThread::idealThreadCount());
}

WorkerPool::WorkerPool(int threads, DispatchMode mode, int maxQueued, Hooks hooks)
    : m_threadCount(qMax(1, threads))
    , m_mode(mode)
    , m_maxQueued(qMax(1, maxQueued))
    , m_hooks(std::move(hooks))
    , m_done(new std::atomic<qint64>[size_t(m_threadCount)])
    , m_busyNs(new std::atomic<qint64>[size_t(m_threadCount)])
{
    for (int i = 0; i < m_threadCount; ++i) {
        m_done[size_t(i)] = 0;
        m_busyNs[size_t(i)] = 0;
    }
    if (m_mode == DispatchMode::RoundRobin || m_mode == DispatchMode::LeastLoaded) {
        for (int i = 0; i < m_threadCount; ++i)
            m_lines.push_back(std::make_unique<Line>());
    }
}

WorkerPool::~WorkerPool()
{
    stop();
}

bool WorkerPool::start(QString *error)
{
    if (m_started)
        return true;
    m_started = true;
    for (int i = 0; i < m_threadCount; ++i) {
        QThread *thread = QThread::create([this, i] { work(i); });
        thread->setObjectName(QStringLiteral("worker-%1").arg(i + 1));
        m_threads.push_back(thread);
        thread->start();
    }

    std::unique_lock<std::mutex> lock(m_startLock);
    m_startDone.wait(lock, [this] { return m_reported == m_threadCount; });
    if (!m_startError.isEmpty()) {
        const QString problem = m_startError;
        lock.unlock();
        stop();
        if (error)
            *error = problem;
        return false;
    }
    return true;
}

void WorkerPool::stop()
{
    if (m_stopping.exchange(true))
        return;

    std::vector<PoolTask> left;
    drain(&left);
    for (PoolTask &task : left) {
        m_queued.fetch_sub(1);
        m_cancelled.fetch_add(1);
        if (task.cancel)
            task.cancel();
    }

    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_ready.notify_all();
    }
    for (const auto &line : m_lines) {
        std::lock_guard<std::mutex> lock(line->lock);
        line->ready.notify_all();
    }
    for (QThread *thread : m_threads) {
        thread->wait();
        delete thread;
    }
    m_threads.clear();
}

void WorkerPool::drain(std::vector<PoolTask> *left)
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        for (PoolTask &task : m_fifo)
            left->push_back(std::move(task));
        m_fifo.clear();
        for (const std::u16string &client : m_turns) {
            auto it = m_byClient.find(client);
            if (it == m_byClient.end())
                continue;
            for (PoolTask &task : it->second)
                left->push_back(std::move(task));
            m_byClient.erase(it);
        }
        m_turns.clear();
        m_size = 0;
    }
    for (const auto &line : m_lines) {
        std::lock_guard<std::mutex> lock(line->lock);
        for (PoolTask &task : line->tasks)
            left->push_back(std::move(task));
        line->size -= int(line->tasks.size());
        line->tasks.clear();
    }
}

bool WorkerPool::submit(PoolTask task)
{
    if (m_stopping.load() || !m_started)
        return false;
    // The line has a limit, and a request past it is refused at once rather
    // than kept: the memory of a server under load stays bounded.
    const int queued = m_queued.fetch_add(1) + 1;
    if (queued > m_maxQueued) {
        m_queued.fetch_sub(1);
        m_refused.fetch_add(1);
        return false;
    }
    raiseTo(m_maxQueuedSeen, queued);
    m_submitted.fetch_add(1);
    task.queuedAtNs = steadyNs();

    switch (m_mode) {
    case DispatchMode::Fifo: {
        std::lock_guard<std::mutex> lock(m_lock);
        m_fifo.push_back(std::move(task));
        ++m_size;
        m_ready.notify_one();
        break;
    }
    case DispatchMode::Fair: {
        std::lock_guard<std::mutex> lock(m_lock);
        const std::u16string client = task.client.toStdU16String();
        std::deque<PoolTask> &own = m_byClient[client];
        if (own.empty())
            m_turns.push_back(client);
        own.push_back(std::move(task));
        ++m_size;
        m_ready.notify_one();
        break;
    }
    case DispatchMode::RoundRobin:
    case DispatchMode::LeastLoaded: {
        size_t target = 0;
        if (m_mode == DispatchMode::RoundRobin) {
            target = m_nextLine.fetch_add(1) % m_lines.size();
        } else {
            // The shortest line, counting the task a thread is busy with: an
            // idle thread with an empty line is the best place of all.
            int best = INT_MAX;
            const size_t first = m_nextLine.fetch_add(1) % m_lines.size();
            for (size_t step = 0; step < m_lines.size(); ++step) {
                const size_t i = (first + step) % m_lines.size();
                const int load = m_lines[i]->size.load();
                if (load < best) {
                    best = load;
                    target = i;
                }
            }
        }
        Line &line = *m_lines[target];
        std::lock_guard<std::mutex> lock(line.lock);
        line.tasks.push_back(std::move(task));
        ++line.size;
        line.ready.notify_one();
        break;
    }
    }
    return true;
}

bool WorkerPool::take(int index, PoolTask *task)
{
    if (m_mode == DispatchMode::RoundRobin || m_mode == DispatchMode::LeastLoaded) {
        Line &line = *m_lines[size_t(index)];
        std::unique_lock<std::mutex> lock(line.lock);
        line.ready.wait(lock, [&] { return m_stopping.load() || !line.tasks.empty(); });
        if (line.tasks.empty())
            return false;
        *task = std::move(line.tasks.front());
        line.tasks.pop_front();
        // The size keeps counting the task until it is done (see runOne()),
        // so that LeastLoaded sees a busy thread as loaded.
        return true;
    }

    std::unique_lock<std::mutex> lock(m_lock);
    m_ready.wait(lock, [this] { return m_stopping.load() || m_size > 0; });
    if (m_size == 0)
        return false;
    if (m_mode == DispatchMode::Fifo) {
        *task = std::move(m_fifo.front());
        m_fifo.pop_front();
    } else {
        const std::u16string client = std::move(m_turns.front());
        m_turns.pop_front();
        auto it = m_byClient.find(client);
        *task = std::move(it->second.front());
        it->second.pop_front();
        if (it->second.empty())
            m_byClient.erase(it);
        else
            m_turns.push_back(client);
    }
    --m_size;
    return true;
}

void WorkerPool::work(int index)
{
    QString problem;
    const bool ready = !m_hooks.started || m_hooks.started(index, &problem);
    {
        std::lock_guard<std::mutex> lock(m_startLock);
        if (!ready && m_startError.isEmpty())
            m_startError = problem.isEmpty() ? QStringLiteral("поток %1 не запустился").arg(index + 1)
                                             : problem;
        ++m_reported;
        m_startDone.notify_all();
    }

    if (ready) {
        PoolTask task;
        while (take(index, &task)) {
            runOne(index, task);
            task = PoolTask();
        }
    }

    if (m_hooks.stopping)
        m_hooks.stopping(index);
}

void WorkerPool::runOne(int index, PoolTask &task)
{
    const qint64 started = steadyNs();
    const qint64 waited = started - task.queuedAtNs;
    m_queued.fetch_sub(1);
    m_busy.fetch_add(1);
    m_startedTasks.fetch_add(1);
    m_waitNs.fetch_add(waited);
    raiseTo(m_maxWaitNs, waited);

    if (task.cancelled && task.cancelled->load()) {
        m_cancelled.fetch_add(1);
        if (task.cancel)
            task.cancel();
    } else {
        // A task that throws - an allocation that failed under memory
        // pressure, most likely - must not take the thread down with it.
        try {
            task.run(index, waited);
        } catch (const std::exception &failure) {
            qWarning().noquote() << QStringLiteral("Поток %1: задача прервана: %2")
                                        .arg(index + 1)
                                        .arg(QString::fromLocal8Bit(failure.what()));
            if (task.cancel)
                task.cancel();
        } catch (...) {
            qWarning().noquote() << QStringLiteral("Поток %1: задача прервана").arg(index + 1);
            if (task.cancel)
                task.cancel();
        }
    }

    const qint64 worked = steadyNs() - started;
    if (!m_lines.empty())
        m_lines[size_t(index)]->size.fetch_sub(1);
    m_busy.fetch_sub(1);
    m_finished.fetch_add(1);
    m_workNs.fetch_add(worked);
    raiseTo(m_maxWorkNs, worked);
    m_done[size_t(index)].fetch_add(1);
    m_busyNs[size_t(index)].fetch_add(worked);
}

PoolSnapshot WorkerPool::snapshot(bool takeMaxima)
{
    PoolSnapshot shot;
    shot.threads = m_threadCount;
    shot.mode = m_mode;
    shot.queued = m_queued.load();
    shot.busy = m_busy.load();
    shot.submitted = m_submitted.load();
    shot.refused = m_refused.load();
    shot.started = m_startedTasks.load();
    shot.finished = m_finished.load();
    shot.cancelled = m_cancelled.load();
    shot.waitNs = m_waitNs.load();
    shot.workNs = m_workNs.load();
    if (takeMaxima) {
        shot.maxWaitNs = m_maxWaitNs.exchange(0);
        shot.maxWorkNs = m_maxWorkNs.exchange(0);
        // What stands in line now is where the next interval starts from.
        shot.maxQueued = m_maxQueuedSeen.exchange(shot.queued);
    } else {
        shot.maxWaitNs = m_maxWaitNs.load();
        shot.maxWorkNs = m_maxWorkNs.load();
        shot.maxQueued = m_maxQueuedSeen.load();
    }
    for (int i = 0; i < m_threadCount; ++i) {
        shot.doneByThread.append(m_done[size_t(i)].load());
        shot.busyNsByThread.append(m_busyNs[size_t(i)].load());
    }
    return shot;
}
