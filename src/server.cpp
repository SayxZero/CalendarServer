#include "server.h"

#include "api.h"
#include "metrics.h"
#include "storage.h"
#include "workerpool.h"

#include <QDebug>
#include <QJsonObject>
#include <QTimer>

Server::Server(const ServerConfig &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
{
}

Server::~Server()
{
    stop();
}

bool Server::start(QString *error)
{
    const auto fail = [this, error](const QString &message) {
        if (error)
            *error = message;
        stop();
        return false;
    };
    if (m_started)
        return true;
    m_started = true;

    const QString path = m_config.databasePath();
    m_storage = std::make_unique<Storage>();
    QString problem;
    if (!m_storage->open(path, &problem, Storage::OpenMode::Create))
        return fail(QStringLiteral("Не удалось открыть базу %1: %2").arg(path, problem));
    // Before any other connection: they take it from the first one.
    m_storage->setDurability(m_config.syncMode == QLatin1String("normal")
                                 ? Storage::Durability::Normal
                                 : Storage::Durability::Full);
    if (m_config.groupCommit > 0 && !m_storage->enableGroupCommit(m_config.groupCommit, &problem))
        return fail(QStringLiteral("Не удалось запустить поток записи: %1").arg(problem));

    // ---- the threads, each with a connection of its own ----------------------
    m_throttle = std::make_shared<LoginThrottle>();
    const int threads = m_config.resolvedThreads();
    m_workers.clear();
    m_workers.resize(size_t(threads));
    WorkerPool::Hooks hooks;
    hooks.started = [this, path](int index, QString *problem) {
        auto storage = std::make_unique<Storage>();
        if (!storage->open(path, problem, Storage::OpenMode::Attach))
            return false;
        Worker &worker = m_workers[size_t(index)];
        worker.api = std::make_unique<Api>(storage.get(), m_config, m_throttle);
        worker.storage = std::move(storage);
        return true;
    };
    hooks.stopping = [this](int index) {
        Worker &worker = m_workers[size_t(index)];
        worker.api.reset();
        worker.storage.reset();
    };
    // One request of a connection is answered at a time, so the line is never
    // longer than the connections; a little more for the upkeep of the base.
    const HttpServer::Limits limits = [this] {
        if (m_tuning.customLimits)
            return m_tuning.limits;
        HttpServer::Limits fromConfig;
        fromConfig.maxBody = qint64(m_config.maxBodyMb) * 1024 * 1024;
        fromConfig.maxConnections = m_config.maxConnections;
        fromConfig.bufferBudget = m_config.resolvedBufferBytes();
        fromConfig.bodyMemoryFactor = ServerConfig::bodyMemoryFactor();
        fromConfig.maxQueueWaitMs = m_config.maxQueueWaitSeconds * 1000;
        return fromConfig;
    }();
    m_pool = std::make_unique<WorkerPool>(threads, dispatchModeFromKey(m_config.dispatch),
                                          limits.maxConnections + 64, hooks);
    if (!m_pool->start(&problem))
        return fail(QStringLiteral("Не удалось запустить потоки: %1").arg(problem));

    // ---- HTTP ------------------------------------------------------------------
    m_http = std::make_unique<HttpServer>();
    m_http->setLimits(limits);
    m_http->setTrustedProxies(m_config.trustedProxies);
    m_http->setPool(m_pool.get(), [this](int worker, const HttpRequest &request) {
        return m_workers[size_t(worker)].api->handle(request);
    });
    if (!m_http->listen(m_config.host, m_config.port)) {
        return fail(QStringLiteral("Не удалось занять порт %1: %2")
                        .arg(m_config.port)
                        .arg(m_http->errorString()));
    }

    // ---- metrics: their absence is no reason not to serve --------------------
    if (m_tuning.metrics) {
        MetricsRecorder::Sources sources;
        sources.pool = [this](bool take) { return m_pool->snapshot(take); };
        sources.http = [this](bool take) { return m_http->snapshot(take); };
        sources.gate = [this] { return m_storage->takeGateStats(); };
        m_metrics = std::make_unique<MetricsRecorder>(
            MetricsStore::pathIn(m_config.resolvedDataDir()), sources, m_tuning.metricsTickMs);
        QJsonObject fixed;
        fixed.insert(QStringLiteral("version"), QStringLiteral(SERVER_VERSION));
        fixed.insert(QStringLiteral("maxBodyMb"), m_config.maxBodyMb);
        fixed.insert(QStringLiteral("maxQueueWaitSeconds"), limits.maxQueueWaitMs / 1000);
        m_metrics->setStatic(fixed);
        if (!m_metrics->start(&problem)) {
            qWarning().noquote() << "Метрики нагрузки не ведутся:" << problem;
            m_metrics.reset();
        }
    }

    // ---- the counted exchanges reach the base every few seconds -------------
    if (m_tuning.flushMs > 0) {
        m_flushTimer = new QTimer(this);
        m_flushTimer->setInterval(m_tuning.flushMs);
        connect(m_flushTimer, &QTimer::timeout, this, &Server::scheduleFlush);
        m_flushTimer->start();
    }

    qInfo().noquote() << QStringLiteral("Потоков: %1 (очередь %2), соединений до %3, "
                                        "память под запросы до %4 МБ, запись %5, %6")
                             .arg(threads)
                             .arg(m_config.dispatch)
                             .arg(limits.maxConnections)
                             .arg(limits.bufferBudget / (1024 * 1024))
                             .arg(m_config.groupCommit > 0
                                      ? QStringLiteral("до %1 запросов за сброс на диск")
                                            .arg(m_config.groupCommit)
                                      : QStringLiteral("каждого запроса отдельно"))
                             .arg(m_config.syncMode == QLatin1String("normal")
                                      ? QStringLiteral("без fsync каждого сброса")
                                      : QStringLiteral("с fsync каждого сброса"));
    return true;
}

void Server::stop()
{
    if (!m_started)
        return;
    m_started = false;
    delete m_flushTimer;
    m_flushTimer = nullptr;
    // The metrics take the last measurement while everything still stands.
    if (m_metrics)
        m_metrics->stop();
    // No new requests; those under way finish on their threads, the rest is
    // dropped. Their answers have nowhere to go: the connections are closed.
    if (m_http)
        m_http->close();
    if (m_pool)
        m_pool->stop();
    // The threads have written all they had; the commit thread goes after them.
    if (m_storage)
        m_storage->disableGroupCommit();
    m_metrics.reset();
    m_http.reset();
    m_pool.reset();
    m_workers.clear();
    if (m_storage)
        m_storage->close();
    m_storage.reset();
}

quint16 Server::port() const
{
    return m_http ? m_http->serverPort() : 0;
}

void Server::flush()
{
    if (m_storage)
        m_storage->flushPending();
}

void Server::scheduleFlush()
{
    // On a thread of the pool: the write may wait at the gate, and this
    // thread would keep every connection waiting meanwhile.
    PoolTask task;
    task.client = QStringLiteral("~upkeep");
    task.run = [this](int worker, qint64) {
        if (Storage *storage = m_workers[size_t(worker)].storage.get())
            storage->flushPending();
    };
    m_pool->submit(std::move(task));
}
