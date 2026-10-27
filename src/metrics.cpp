#include "metrics.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVariant>

#include <chrono>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#elif defined(Q_OS_LINUX)
#include <unistd.h>
#endif

namespace {

qint64 steadyNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

#ifdef Q_OS_WIN
quint64 fileTime(const FILETIME &time)
{
    return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
#endif

#ifdef Q_OS_LINUX
QByteArray readProc(const char *path)
{
    QFile file(QString::fromLatin1(path));
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

/*! A "Name:   1234 kB" line of /proc/meminfo, in bytes. */
qint64 memInfo(const QByteArray &text, const char *name)
{
    const QByteArray key = QByteArray(name) + ':';
    const int at = text.indexOf(key);
    if (at < 0)
        return -1;
    const int end = text.indexOf('\n', at);
    const QList<QByteArray> parts = text.mid(at + key.size(), end - at - key.size()).simplified().split(' ');
    return parts.isEmpty() ? -1 : parts.first().toLongLong() * 1024;
}
#endif

/*!
 * The columns of one row of the history as a bucket of rows adds them up:
 * counts are summed, averages weighted by the minutes behind each row, an
 * unknown processor or memory (-1) left out of its average.
 */
QString aggregateColumns()
{
    const auto weighted = [](const char *column) {
        return QStringLiteral("COALESCE(SUM(CASE WHEN %1 >= 0 THEN %1 * minutes END)"
                              " / NULLIF(SUM(CASE WHEN %1 >= 0 THEN minutes END), 0), -1)")
            .arg(QLatin1String(column));
    };
    return QStringLiteral("SUM(minutes), SUM(requests), SUM(client_errors), SUM(server_errors),"
                          " SUM(overloaded), SUM(wait_count), SUM(wait_sum_ms), MAX(wait_max_ms),"
                          " SUM(work_count), SUM(work_sum_ms), MAX(work_max_ms),"
                          " SUM(queue_avg * minutes) / NULLIF(SUM(minutes), 0), MAX(queue_max),"
                          " SUM(busy_avg * minutes) / NULLIF(SUM(minutes), 0), MAX(threads),"
                          " SUM(conn_avg * minutes) / NULLIF(SUM(minutes), 0), MAX(conn_max),"
                          " %1, %2, %3, %4,"
                          " SUM(bytes_in), SUM(bytes_out), SUM(gate_count), SUM(gate_wait_sum_ms),"
                          " MAX(gate_wait_max_ms), SUM(commits)")
        .arg(weighted("cpu_process"), weighted("cpu_system"), weighted("memory_process"),
             weighted("memory_load"));
}

const char *const kColumns =
    "minutes, requests, client_errors, server_errors, overloaded, wait_count, wait_sum_ms,"
    " wait_max_ms, work_count, work_sum_ms, work_max_ms, queue_avg, queue_max, busy_avg, threads,"
    " conn_avg, conn_max, cpu_process, cpu_system, memory_process, memory_load, bytes_in,"
    " bytes_out, gate_count, gate_wait_sum_ms, gate_wait_max_ms, commits";

/*! Fills @p row from the columns of kColumns starting at @p first. */
void readColumns(const QSqlQuery &query, int first, MetricsRow *row)
{
    int i = first;
    const auto next = [&]() { return query.value(i++); };
    row->minutes = next().toInt();
    row->requests = next().toLongLong();
    row->clientErrors = next().toLongLong();
    row->serverErrors = next().toLongLong();
    row->overloaded = next().toLongLong();
    row->waitCount = next().toLongLong();
    row->waitSumMs = next().toDouble();
    row->waitMaxMs = next().toDouble();
    row->workCount = next().toLongLong();
    row->workSumMs = next().toDouble();
    row->workMaxMs = next().toDouble();
    row->queueAvg = next().toDouble();
    row->queueMax = next().toInt();
    row->busyAvg = next().toDouble();
    row->threads = next().toInt();
    row->connectionsAvg = next().toDouble();
    row->connectionsMax = next().toInt();
    const QVariant cpuProcess = next();
    const QVariant cpuSystem = next();
    const QVariant memory = next();
    const QVariant memoryLoad = next();
    row->cpuProcess = cpuProcess.isNull() ? -1 : cpuProcess.toDouble();
    row->cpuSystem = cpuSystem.isNull() ? -1 : cpuSystem.toDouble();
    row->memoryProcess = memory.isNull() ? -1 : memory.toDouble();
    row->memoryLoad = memoryLoad.isNull() ? -1 : memoryLoad.toDouble();
    row->bytesIn = next().toLongLong();
    row->bytesOut = next().toLongLong();
    row->gateCount = next().toLongLong();
    row->gateWaitSumMs = next().toDouble();
    row->gateWaitMaxMs = next().toDouble();
    row->commits = next().toLongLong();
}

} // namespace

// --------------------------------------------------------------------- probe

ResourceProbe::Sample ResourceProbe::sample()
{
    Sample sample;
    const qint64 wallNs = steadyNs();
    qint64 processNs = -1;
    quint64 systemBusy = 0;
    quint64 systemTotal = 0;
    bool haveSystem = false;

#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        processNs = qint64((fileTime(kernel) + fileTime(user)) * 100);
    FILETIME idle, systemKernel, systemUser;
    if (GetSystemTimes(&idle, &systemKernel, &systemUser)) {
        // The kernel time of the system includes its idle time.
        systemTotal = fileTime(systemKernel) + fileTime(systemUser);
        systemBusy = systemTotal - fileTime(idle);
        haveSystem = true;
    }
    PROCESS_MEMORY_COUNTERS counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        sample.memoryProcess = qint64(counters.WorkingSetSize);
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        sample.memoryTotal = qint64(status.ullTotalPhys);
        sample.memoryLoad = status.ullTotalPhys > 0
                                ? 100.0 * double(status.ullTotalPhys - status.ullAvailPhys)
                                      / double(status.ullTotalPhys)
                                : -1;
    }
#elif defined(Q_OS_LINUX)
    static const long ticksPerSecond = sysconf(_SC_CLK_TCK);
    static const long pageSize = sysconf(_SC_PAGESIZE);
    const QByteArray stat = readProc("/proc/self/stat");
    // The name of the program is in brackets and may hold spaces: the
    // fields are counted from the closing one (utime and stime are 14, 15).
    const int close = stat.lastIndexOf(')');
    if (close > 0 && ticksPerSecond > 0) {
        const QList<QByteArray> fields = stat.mid(close + 2).split(' ');
        if (fields.size() > 12) {
            processNs = (fields.at(11).toLongLong() + fields.at(12).toLongLong()) * 1000000000LL
                        / ticksPerSecond;
        }
    }
    const QByteArray cpu = readProc("/proc/stat");
    const QList<QByteArray> values = cpu.left(cpu.indexOf('\n')).simplified().split(' ');
    if (values.size() > 8 && values.first() == "cpu") {
        for (int i = 1; i <= 8; ++i)
            systemTotal += values.at(i).toULongLong();
        const quint64 idle = values.at(4).toULongLong() + values.at(5).toULongLong();
        systemBusy = systemTotal - idle;
        haveSystem = true;
    }
    const QList<QByteArray> pages = readProc("/proc/self/statm").simplified().split(' ');
    if (pages.size() > 1 && pageSize > 0)
        sample.memoryProcess = pages.at(1).toLongLong() * pageSize;
    const QByteArray meminfo = readProc("/proc/meminfo");
    const qint64 total = memInfo(meminfo, "MemTotal");
    const qint64 available = memInfo(meminfo, "MemAvailable");
    if (total > 0) {
        sample.memoryTotal = total;
        if (available >= 0)
            sample.memoryLoad = 100.0 * double(total - available) / double(total);
    }
#endif

    if (m_primed) {
        const qint64 wall = wallNs - m_wallNs;
        const int cores = qMax(1, QThread::idealThreadCount());
        if (processNs >= 0 && m_processNs >= 0 && wall > 0) {
            sample.cpuProcess =
                qBound(0.0, 100.0 * double(processNs - m_processNs) / double(wall) / cores, 100.0);
        }
        if (haveSystem && systemTotal > m_systemTotal) {
            sample.cpuSystem = qBound(0.0,
                                      100.0 * double(systemBusy - m_systemBusy)
                                          / double(systemTotal - m_systemTotal),
                                      100.0);
        }
    }
    m_wallNs = wallNs;
    m_processNs = processNs;
    m_systemBusy = systemBusy;
    m_systemTotal = systemTotal;
    m_primed = true;
    return sample;
}

// --------------------------------------------------------------------- store

QString MetricsStore::pathIn(const QString &dataDir)
{
    return QDir(dataDir).absoluteFilePath(QStringLiteral("metrics.db"));
}

MetricsStore::MetricsStore()
    : m_connection(QStringLiteral("metrics-%1").arg(QUuid::createUuid().toString(QUuid::Id128)))
{
}

MetricsStore::~MetricsStore()
{
    close();
}

bool MetricsStore::open(const QString &path, bool write, QString *error)
{
    close();
    const auto fail = [this, error](const QString &message) {
        m_lastError = message;
        if (error)
            *error = message;
        close();
        return false;
    };
    if (!write && !QFileInfo(path).isFile())
        return fail(QStringLiteral("истории нагрузки ещё нет: её ведёт сервер версии 1.3"));
    if (write && !QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(QStringLiteral("не удалось создать папку %1").arg(QFileInfo(path).absolutePath()));

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    m_db.setDatabaseName(path);
    m_db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=3000"));
    if (!m_db.open())
        return fail(m_db.lastError().text());

    QSqlQuery query(m_db);
    if (write) {
        // The history is not worth an fsync a tick: what a power cut takes
        // is the last few seconds of it.
        const QStringList statements = {
            QStringLiteral("PRAGMA journal_mode = WAL"),
            QStringLiteral("PRAGMA synchronous = NORMAL"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS samples ("
                           "span INTEGER NOT NULL, t INTEGER NOT NULL,"
                           "minutes INTEGER NOT NULL DEFAULT 1,"
                           "requests INTEGER NOT NULL DEFAULT 0,"
                           "client_errors INTEGER NOT NULL DEFAULT 0,"
                           "server_errors INTEGER NOT NULL DEFAULT 0,"
                           "overloaded INTEGER NOT NULL DEFAULT 0,"
                           "wait_count INTEGER NOT NULL DEFAULT 0, wait_sum_ms REAL NOT NULL DEFAULT 0,"
                           "wait_max_ms REAL NOT NULL DEFAULT 0,"
                           "work_count INTEGER NOT NULL DEFAULT 0, work_sum_ms REAL NOT NULL DEFAULT 0,"
                           "work_max_ms REAL NOT NULL DEFAULT 0,"
                           "queue_avg REAL NOT NULL DEFAULT 0, queue_max INTEGER NOT NULL DEFAULT 0,"
                           "busy_avg REAL NOT NULL DEFAULT 0, threads INTEGER NOT NULL DEFAULT 0,"
                           "conn_avg REAL NOT NULL DEFAULT 0, conn_max INTEGER NOT NULL DEFAULT 0,"
                           "cpu_process REAL NOT NULL DEFAULT -1, cpu_system REAL NOT NULL DEFAULT -1,"
                           "memory_process REAL NOT NULL DEFAULT -1,"
                           "memory_load REAL NOT NULL DEFAULT -1,"
                           "bytes_in INTEGER NOT NULL DEFAULT 0, bytes_out INTEGER NOT NULL DEFAULT 0,"
                           "gate_count INTEGER NOT NULL DEFAULT 0,"
                           "gate_wait_sum_ms REAL NOT NULL DEFAULT 0,"
                           "gate_wait_max_ms REAL NOT NULL DEFAULT 0,"
                           "commits INTEGER NOT NULL DEFAULT 0,"
                           "PRIMARY KEY (span, t))"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS live ("
                           "id INTEGER PRIMARY KEY CHECK (id = 1),"
                           "at INTEGER NOT NULL, data TEXT NOT NULL)"),
            QStringLiteral("PRAGMA user_version = 1"),
        };
        for (const QString &statement : statements) {
            if (!query.exec(statement))
                return fail(query.lastError().text());
        }
    } else {
        if (!query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type = 'table'"
                                       " AND name = 'samples'"))
            || !query.next()) {
            return fail(QStringLiteral("это не история нагрузки сервера"));
        }
    }
    return true;
}

void MetricsStore::close()
{
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase();
    if (QSqlDatabase::contains(m_connection))
        QSqlDatabase::removeDatabase(m_connection);
}

bool MetricsStore::isOpen() const
{
    return m_db.isOpen();
}

bool MetricsStore::writeRow(const MetricsRow &row)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO samples (span, t, %1) VALUES (:span, :t,"
                                 " :minutes, :requests, :client_errors, :server_errors, :overloaded,"
                                 " :wait_count, :wait_sum_ms, :wait_max_ms, :work_count, :work_sum_ms,"
                                 " :work_max_ms, :queue_avg, :queue_max, :busy_avg, :threads,"
                                 " :conn_avg, :conn_max, :cpu_process, :cpu_system, :memory_process,"
                                 " :memory_load, :bytes_in, :bytes_out, :gate_count,"
                                 " :gate_wait_sum_ms, :gate_wait_max_ms, :commits)")
                      .arg(QLatin1String(kColumns)));
    query.bindValue(QStringLiteral(":span"), row.span);
    query.bindValue(QStringLiteral(":t"), row.t);
    query.bindValue(QStringLiteral(":minutes"), qMax(1, row.minutes));
    query.bindValue(QStringLiteral(":requests"), row.requests);
    query.bindValue(QStringLiteral(":client_errors"), row.clientErrors);
    query.bindValue(QStringLiteral(":server_errors"), row.serverErrors);
    query.bindValue(QStringLiteral(":overloaded"), row.overloaded);
    query.bindValue(QStringLiteral(":wait_count"), row.waitCount);
    query.bindValue(QStringLiteral(":wait_sum_ms"), row.waitSumMs);
    query.bindValue(QStringLiteral(":wait_max_ms"), row.waitMaxMs);
    query.bindValue(QStringLiteral(":work_count"), row.workCount);
    query.bindValue(QStringLiteral(":work_sum_ms"), row.workSumMs);
    query.bindValue(QStringLiteral(":work_max_ms"), row.workMaxMs);
    query.bindValue(QStringLiteral(":queue_avg"), row.queueAvg);
    query.bindValue(QStringLiteral(":queue_max"), row.queueMax);
    query.bindValue(QStringLiteral(":busy_avg"), row.busyAvg);
    query.bindValue(QStringLiteral(":threads"), row.threads);
    query.bindValue(QStringLiteral(":conn_avg"), row.connectionsAvg);
    query.bindValue(QStringLiteral(":conn_max"), row.connectionsMax);
    query.bindValue(QStringLiteral(":cpu_process"), row.cpuProcess);
    query.bindValue(QStringLiteral(":cpu_system"), row.cpuSystem);
    query.bindValue(QStringLiteral(":memory_process"), row.memoryProcess);
    query.bindValue(QStringLiteral(":memory_load"), row.memoryLoad);
    query.bindValue(QStringLiteral(":bytes_in"), row.bytesIn);
    query.bindValue(QStringLiteral(":bytes_out"), row.bytesOut);
    query.bindValue(QStringLiteral(":gate_count"), row.gateCount);
    query.bindValue(QStringLiteral(":gate_wait_sum_ms"), row.gateWaitSumMs);
    query.bindValue(QStringLiteral(":gate_wait_max_ms"), row.gateWaitMaxMs);
    query.bindValue(QStringLiteral(":commits"), row.commits);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool MetricsStore::rollUpHour(qint64 hour)
{
    hour = hour / 3600 * 3600;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO samples (span, t, %1)"
                                 " SELECT 3600, :hour, %2 FROM samples"
                                 " WHERE span = 60 AND t >= :from AND t < :to HAVING COUNT(*) > 0")
                      .arg(QLatin1String(kColumns), aggregateColumns()));
    query.bindValue(QStringLiteral(":hour"), hour);
    query.bindValue(QStringLiteral(":from"), hour);
    query.bindValue(QStringLiteral(":to"), hour + 3600);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool MetricsStore::prune(qint64 now)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM samples WHERE (span = 60 AND t < :minutes)"
                                 " OR (span = 3600 AND t < :hours)"));
    query.bindValue(QStringLiteral(":minutes"), now - qint64(minuteRowsDays()) * 86400);
    query.bindValue(QStringLiteral(":hours"), now - qint64(hourRowsDays()) * 86400);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool MetricsStore::writeLive(const QJsonObject &live)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO live (id, at, data) VALUES (1, :at, :data)"));
    query.bindValue(QStringLiteral(":at"), qint64(live.value(QStringLiteral("at")).toDouble()));
    query.bindValue(QStringLiteral(":data"),
                    QString::fromUtf8(QJsonDocument(live).toJson(QJsonDocument::Compact)));
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

QJsonObject MetricsStore::live()
{
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT at, data FROM live WHERE id = 1")) || !query.next())
        return QJsonObject();
    QJsonObject object = QJsonDocument::fromJson(query.value(1).toString().toUtf8()).object();
    object.insert(QStringLiteral("at"), double(query.value(0).toLongLong()));
    return object;
}

QList<MetricsRow> MetricsStore::series(qint64 from, qint64 to, int bucket, int utcOffset)
{
    QList<MetricsRow> rows;
    bucket = qMax(60, bucket);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const bool minutes =
        bucket < 3600 && from >= now - qint64(minuteRowsDays()) * 86400 - 60;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT ((t + :offset) / :bucket) * :bucket - :offset2 AS b, %1"
                                 " FROM samples WHERE span = :span AND t >= :from AND t < :to"
                                 " GROUP BY b ORDER BY b")
                      .arg(aggregateColumns()));
    query.bindValue(QStringLiteral(":offset"), utcOffset);
    query.bindValue(QStringLiteral(":offset2"), utcOffset);
    query.bindValue(QStringLiteral(":bucket"), bucket);
    query.bindValue(QStringLiteral(":span"), minutes ? 60 : 3600);
    query.bindValue(QStringLiteral(":from"), from);
    query.bindValue(QStringLiteral(":to"), to);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return rows;
    }
    while (query.next()) {
        MetricsRow row;
        row.t = query.value(0).toLongLong();
        row.span = bucket;
        readColumns(query, 1, &row);
        rows.append(row);
    }
    return rows;
}

qint64 MetricsStore::firstTime()
{
    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("SELECT MIN(t) FROM samples")) && query.next())
        return query.value(0).toLongLong();
    return 0;
}

// ------------------------------------------------------------------ recorder

MetricsRecorder::MetricsRecorder(const QString &path, Sources sources, int tickMs)
    : m_path(path)
    , m_sources(std::move(sources))
    , m_tickMs(tickMs)
{
}

MetricsRecorder::~MetricsRecorder()
{
    stop();
}

bool MetricsRecorder::start(QString *error)
{
    if (m_thread)
        return true;
    m_thread = new QThread;
    m_thread->setObjectName(QStringLiteral("metrics"));
    m_context = new QObject;
    m_context->moveToThread(m_thread);
    m_thread->start();

    bool ok = false;
    QString problem;
    QMetaObject::invokeMethod(m_context, [&] { openOnThread(&problem, &ok); },
                              Qt::BlockingQueuedConnection);
    if (!ok) {
        stop();
        if (error)
            *error = problem;
        return false;
    }
    return true;
}

void MetricsRecorder::stop()
{
    if (!m_thread)
        return;
    QMetaObject::invokeMethod(m_context, [this] { closeOnThread(); }, Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait();
    delete m_context;
    m_context = nullptr;
    delete m_thread;
    m_thread = nullptr;
}

void MetricsRecorder::openOnThread(QString *error, bool *ok)
{
    m_store = new MetricsStore;
    if (!m_store->open(m_path, true, error)) {
        delete m_store;
        m_store = nullptr;
        *ok = false;
        return;
    }
    m_startedAt = QDateTime::currentSecsSinceEpoch();
    m_store->prune(m_startedAt);
    m_lastPrune = m_startedAt;
    m_probe.sample();
    if (m_tickMs > 0) {
        m_timer = new QTimer(m_context);
        m_timer->setInterval(m_tickMs);
        QObject::connect(m_timer, &QTimer::timeout, m_context,
                         [this] { measure(QDateTime::currentSecsSinceEpoch()); });
        m_timer->start();
    }
    *ok = true;
}

void MetricsRecorder::closeOnThread()
{
    delete m_timer;
    m_timer = nullptr;
    if (m_store) {
        // The last, unfinished minute is kept too: the server was there for it.
        measure(QDateTime::currentSecsSinceEpoch());
        if (m_minute.t != 0)
            closeMinute();
        QJsonObject live = m_store->live();
        live.insert(QStringLiteral("stopped"), true);
        m_store->writeLive(live);
        delete m_store;
        m_store = nullptr;
    }
}

void MetricsRecorder::tick(qint64 now)
{
    if (!m_context)
        return;
    QMetaObject::invokeMethod(m_context, [this, now] { measure(now); }, Qt::BlockingQueuedConnection);
}

void MetricsRecorder::flushMinute()
{
    if (!m_context)
        return;
    QMetaObject::invokeMethod(m_context, [this] {
        if (m_minute.t != 0)
            closeMinute();
    }, Qt::BlockingQueuedConnection);
}

void MetricsRecorder::resetMinute(qint64 start)
{
    m_minute = MetricsRow();
    m_minute.t = start;
    m_minute.span = 60;
    m_minute.minutes = 1;
    m_samples = 0;
    m_queueSum = 0;
    m_busySum = 0;
    m_connectionsSum = 0;
    m_cpuProcessSum = 0;
    m_cpuProcessSamples = 0;
    m_cpuSystemSum = 0;
    m_cpuSystemSamples = 0;
    m_memorySum = 0;
    m_memoryLoadSum = 0;
    m_memorySamples = 0;
    m_memoryLoadSamples = 0;
}

void MetricsRecorder::closeMinute()
{
    if (!m_store || m_minute.t == 0)
        return;
    MetricsRow row = m_minute;
    if (m_samples > 0) {
        row.queueAvg = m_queueSum / m_samples;
        row.busyAvg = m_busySum / m_samples;
        row.connectionsAvg = m_connectionsSum / m_samples;
    }
    row.cpuProcess = m_cpuProcessSamples > 0 ? m_cpuProcessSum / m_cpuProcessSamples : -1;
    row.cpuSystem = m_cpuSystemSamples > 0 ? m_cpuSystemSum / m_cpuSystemSamples : -1;
    row.memoryProcess = m_memorySamples > 0 ? m_memorySum / m_memorySamples : -1;
    row.memoryLoad = m_memoryLoadSamples > 0 ? m_memoryLoadSum / m_memoryLoadSamples : -1;
    if (!m_store->writeRow(row) || !m_store->rollUpHour(row.t))
        qWarning().noquote() << "История нагрузки не записана:" << m_store->lastError();
    m_minute.t = 0;

    const qint64 now = row.t + 60;
    if (now - m_lastPrune >= 3600) {
        m_store->prune(now);
        m_lastPrune = now;
    }
}

void MetricsRecorder::measure(qint64 now)
{
    if (!m_store)
        return;
    const PoolSnapshot pool = m_sources.pool ? m_sources.pool(true) : PoolSnapshot();
    const HttpSnapshot http = m_sources.http ? m_sources.http(true) : HttpSnapshot();
    const GateStats gate = m_sources.gate ? m_sources.gate() : GateStats();
    const ResourceProbe::Sample resources = m_probe.sample();

    // A tick covers the seconds before it: they belong to the minute they are in.
    const qint64 minute = (now - 1) / 60 * 60;
    if (m_minute.t != 0 && m_minute.t != minute)
        closeMinute();
    if (m_minute.t == 0)
        resetMinute(minute);

    Tick tick;
    tick.at = now;
    if (m_haveLast) {
        tick.requests = http.requests - m_lastHttp.requests;
        tick.overloaded = http.overloaded - m_lastHttp.overloaded;
        tick.waitCount = pool.started - m_lastPool.started;
        tick.waitSumMs = double(pool.waitNs - m_lastPool.waitNs) / 1e6;
        tick.workCount = pool.finished - m_lastPool.finished;
        tick.workSumMs = double(pool.workNs - m_lastPool.workNs) / 1e6;
        m_minute.requests += tick.requests;
        m_minute.clientErrors += http.clientErrors - m_lastHttp.clientErrors;
        m_minute.serverErrors += http.serverErrors - m_lastHttp.serverErrors;
        m_minute.overloaded += tick.overloaded;
        m_minute.waitCount += tick.waitCount;
        m_minute.waitSumMs += tick.waitSumMs;
        m_minute.workCount += tick.workCount;
        m_minute.workSumMs += tick.workSumMs;
        m_minute.bytesIn += http.bytesIn - m_lastHttp.bytesIn;
        m_minute.bytesOut += http.bytesOut - m_lastHttp.bytesOut;
        m_minute.gateCount += gate.writes - m_lastGate.writes;
        m_minute.commits += gate.commits - m_lastGate.commits;
        m_minute.gateWaitSumMs += double(gate.waitNs - m_lastGate.waitNs) / 1e6;
    }
    m_minute.waitMaxMs = qMax(m_minute.waitMaxMs, double(pool.maxWaitNs) / 1e6);
    m_minute.workMaxMs = qMax(m_minute.workMaxMs, double(pool.maxWorkNs) / 1e6);
    m_minute.gateWaitMaxMs = qMax(m_minute.gateWaitMaxMs, double(gate.maxWaitNs) / 1e6);
    m_minute.queueMax = qMax(m_minute.queueMax, qMax(pool.maxQueued, pool.queued));
    m_minute.connectionsMax = qMax(m_minute.connectionsMax, qMax(http.maxConnections, http.connections));
    m_minute.threads = pool.threads;
    ++m_samples;
    m_queueSum += pool.queued;
    m_busySum += pool.busy;
    m_connectionsSum += http.connections;
    if (resources.cpuProcess >= 0) {
        m_cpuProcessSum += resources.cpuProcess;
        ++m_cpuProcessSamples;
    }
    if (resources.cpuSystem >= 0) {
        m_cpuSystemSum += resources.cpuSystem;
        ++m_cpuSystemSamples;
    }
    if (resources.memoryProcess >= 0) {
        m_memorySum += double(resources.memoryProcess);
        ++m_memorySamples;
    }
    if (resources.memoryLoad >= 0) {
        m_memoryLoadSum += resources.memoryLoad;
        ++m_memoryLoadSamples;
    }

    m_recent.push_back(tick);
    while (!m_recent.empty() && m_recent.front().at <= now - 60)
        m_recent.pop_front();

    // ---- the live values: now, and the last minute ------------------------
    Tick minuteTotal;
    for (const Tick &one : m_recent) {
        minuteTotal.requests += one.requests;
        minuteTotal.overloaded += one.overloaded;
        minuteTotal.waitCount += one.waitCount;
        minuteTotal.waitSumMs += one.waitSumMs;
        minuteTotal.workCount += one.workCount;
        minuteTotal.workSumMs += one.workSumMs;
    }
    const qint64 window = qBound(qint64(1), now - m_startedAt, qint64(60));

    QJsonObject live = m_static;
    live.insert(QStringLiteral("at"), double(now));
    live.insert(QStringLiteral("startedAt"), double(m_startedAt));
    live.insert(QStringLiteral("threads"), pool.threads);
    live.insert(QStringLiteral("mode"), dispatchModeKey(pool.mode));
    live.insert(QStringLiteral("queued"), pool.queued);
    live.insert(QStringLiteral("queuedMax"), m_minute.queueMax);
    live.insert(QStringLiteral("busy"), pool.busy);
    live.insert(QStringLiteral("connections"), http.connections);
    live.insert(QStringLiteral("connectionLimit"), http.connectionLimit);
    live.insert(QStringLiteral("paused"), http.paused);
    live.insert(QStringLiteral("requestsPerMinute"), double(minuteTotal.requests) * 60.0 / double(window));
    live.insert(QStringLiteral("overloadedLastMinute"), double(minuteTotal.overloaded));
    live.insert(QStringLiteral("waitAvgMs"),
                minuteTotal.waitCount > 0 ? minuteTotal.waitSumMs / double(minuteTotal.waitCount) : 0.0);
    live.insert(QStringLiteral("workAvgMs"),
                minuteTotal.workCount > 0 ? minuteTotal.workSumMs / double(minuteTotal.workCount) : 0.0);
    live.insert(QStringLiteral("waitMaxMs"), m_minute.waitMaxMs);
    live.insert(QStringLiteral("workMaxMs"), m_minute.workMaxMs);
    live.insert(QStringLiteral("requestsTotal"), double(http.requests));
    live.insert(QStringLiteral("overloadedTotal"), double(http.overloaded));
    live.insert(QStringLiteral("serverErrorsTotal"), double(http.serverErrors));
    live.insert(QStringLiteral("buffered"), double(http.buffered));
    live.insert(QStringLiteral("bufferBudget"), double(http.bufferBudget));
    live.insert(QStringLiteral("cpuProcess"), resources.cpuProcess);
    live.insert(QStringLiteral("cpuSystem"), resources.cpuSystem);
    live.insert(QStringLiteral("memoryProcess"), double(resources.memoryProcess));
    live.insert(QStringLiteral("memoryTotal"), double(resources.memoryTotal));
    live.insert(QStringLiteral("memoryLoad"), resources.memoryLoad);
    live.insert(QStringLiteral("gateWrites"), double(gate.writes));
    live.insert(QStringLiteral("writesPerCommit"),
                gate.commits > 0 ? double(gate.writes) / double(gate.commits) : 0.0);
    live.insert(QStringLiteral("gateWaitAvgMs"),
                gate.writes > 0 ? double(gate.waitNs) / 1e6 / double(gate.writes) : 0.0);
    QJsonArray done;
    QJsonArray busy;
    for (int i = 0; i < pool.doneByThread.size(); ++i) {
        done.append(double(pool.doneByThread.at(i)));
        busy.append(double(pool.busyNsByThread.value(i)) / 1e6);
    }
    live.insert(QStringLiteral("doneByThread"), done);
    live.insert(QStringLiteral("busyMsByThread"), busy);
    if (!m_store->writeLive(live))
        qWarning().noquote() << "Текущая нагрузка не записана:" << m_store->lastError();

    m_lastPool = pool;
    m_lastHttp = http;
    m_lastGate = gate;
    m_lastResources = resources;
    m_haveLast = true;
}
