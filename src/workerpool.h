#pragma once

#include <QList>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

class QThread;

/*!
 * How requests waiting for a thread are lined up.
 *
 * Fair - one line that every idle thread takes from, served by turns of the
 * clients: a client with fifty requests waiting does not keep one with a
 * single request behind all of them. The default: the benchmark in DesDoc
 * (section "Потоки") explains why.
 *
 * Fifo - the same single line, in the order of arrival.
 *
 * RoundRobin, LeastLoaded - a line per thread, filled by turns or into the
 * shortest one. Kept for comparison: a thread busy with a long request holds
 * up the requests that landed in its line while other threads idle.
 */
enum class DispatchMode { Fair, Fifo, RoundRobin, LeastLoaded };

QString dispatchModeKey(DispatchMode mode);
DispatchMode dispatchModeFromKey(const QString &key, bool *ok = nullptr);

/*! One piece of work for the pool. */
struct PoolTask
{
    /*! Whose turn it is in the fair line: a session token, else the address. */
    QString client;

    /*!
     * The work, on a thread of the pool: @p worker is the index of the thread,
     * @p waitedNs how long the task stood in the line.
     */
    std::function<void(int worker, qint64 waitedNs)> run;

    /*!
     * Called instead of run() when the task is not going to run: @p cancelled
     * was raised while it waited (the client went away), or the pool stops.
     * On a thread of the pool or on the one that stops it.
     */
    std::function<void()> cancel;
    std::shared_ptr<std::atomic<bool>> cancelled;

    qint64 queuedAtNs = 0;   //!< set by the pool
};

/*!
 * What the pool has done so far, for the metrics; any thread may ask. The
 * totals only grow; the maxima are those since the previous snapshot that
 * took them.
 */
struct PoolSnapshot
{
    int threads = 0;
    DispatchMode mode = DispatchMode::Fair;
    int queued = 0;           //!< standing in line right now
    int busy = 0;             //!< threads at work right now
    qint64 submitted = 0;
    qint64 refused = 0;       //!< the line was full
    qint64 started = 0;
    qint64 finished = 0;
    qint64 cancelled = 0;
    qint64 waitNs = 0;        //!< in line, all tasks together
    qint64 workNs = 0;        //!< at work, all tasks together
    qint64 maxWaitNs = 0;
    qint64 maxWorkNs = 0;
    int maxQueued = 0;
    QList<qint64> doneByThread;
    QList<qint64> busyNsByThread;
};

/*!
 * A fixed set of worker threads and the line of tasks in front of them.
 *
 * Each thread is set up by Hooks::started on itself before it takes its
 * first task - the server opens a connection to the base there, one per
 * thread - and taken down by Hooks::stopping on itself as well.
 */
class WorkerPool
{
public:
    struct Hooks
    {
        /*! On the new thread; false fails start(). */
        std::function<bool(int worker, QString *error)> started;
        std::function<void(int worker)> stopping;
    };

    /*! The number of threads the processor runs at once, but at least two. */
    static int defaultThreads();

    WorkerPool(int threads, DispatchMode mode, int maxQueued, Hooks hooks = Hooks());
    ~WorkerPool();

    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

    /*! Starts the threads and waits until each one is set up. */
    bool start(QString *error = nullptr);

    /*!
     * Cancels what still waits, lets the running tasks finish and stops the
     * threads. The destructor does it too.
     */
    void stop();

    /*! Puts @p task in line; false when the line is full or the pool stopped. */
    bool submit(PoolTask task);

    int threadCount() const { return m_threadCount; }
    DispatchMode mode() const { return m_mode; }
    int queued() const { return m_queued.load(); }
    int busy() const { return m_busy.load(); }

    /*! @p takeMaxima resets the maxima, for the next interval of the metrics. */
    PoolSnapshot snapshot(bool takeMaxima = false);

private:
    struct Line
    {
        std::mutex lock;
        std::condition_variable ready;
        std::deque<PoolTask> tasks;
        std::atomic<int> size { 0 };
    };

    void work(int index);
    bool take(int index, PoolTask *task);
    void runOne(int index, PoolTask &task);
    void drain(std::vector<PoolTask> *left);

    const int m_threadCount;
    const DispatchMode m_mode;
    const int m_maxQueued;
    Hooks m_hooks;

    // Fair and Fifo: one line under m_lock. Fair keeps a line per client and
    // the order of their turns.
    std::mutex m_lock;
    std::condition_variable m_ready;
    std::deque<PoolTask> m_fifo;
    std::unordered_map<std::u16string, std::deque<PoolTask>> m_byClient;
    std::deque<std::u16string> m_turns;
    int m_size = 0;

    // RoundRobin and LeastLoaded: a line per thread.
    std::vector<std::unique_ptr<Line>> m_lines;
    std::atomic<unsigned> m_nextLine { 0 };

    std::atomic<bool> m_stopping { false };
    bool m_started = false;
    std::vector<QThread *> m_threads;

    std::atomic<int> m_queued { 0 };
    std::atomic<int> m_busy { 0 };
    std::atomic<qint64> m_submitted { 0 };
    std::atomic<qint64> m_refused { 0 };
    std::atomic<qint64> m_startedTasks { 0 };
    std::atomic<qint64> m_finished { 0 };
    std::atomic<qint64> m_cancelled { 0 };
    std::atomic<qint64> m_waitNs { 0 };
    std::atomic<qint64> m_workNs { 0 };
    std::atomic<qint64> m_maxWaitNs { 0 };
    std::atomic<qint64> m_maxWorkNs { 0 };
    std::atomic<int> m_maxQueuedSeen { 0 };
    std::unique_ptr<std::atomic<qint64>[]> m_done;
    std::unique_ptr<std::atomic<qint64>[]> m_busyNs;

    // start(): the threads report in.
    std::mutex m_startLock;
    std::condition_variable m_startDone;
    int m_reported = 0;
    QString m_startError;
};
