#pragma once

#include "httpserver.h"
#include "storage.h"
#include "workerpool.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

#include <deque>
#include <functional>

class QThread;
class QTimer;

/*!
 * The processor and the memory: of the server process and of the whole
 * machine. Windows and Linux; elsewhere the values stay unknown (-1).
 */
class ResourceProbe
{
public:
    struct Sample
    {
        double cpuProcess = -1;      //!< % of all the cores, since the previous sample
        double cpuSystem = -1;       //!< % of the machine busy, since the previous sample
        qint64 memoryProcess = -1;   //!< bytes the process holds in RAM (working set, RSS)
        qint64 memoryTotal = -1;     //!< bytes of RAM of the machine
        double memoryLoad = -1;      //!< % of that RAM in use by everybody
    };

    /*! The first sample knows the memory; the processor from the second on. */
    Sample sample();

private:
    qint64 m_wallNs = 0;
    qint64 m_processNs = 0;
    quint64 m_systemBusy = 0;
    quint64 m_systemTotal = 0;
    bool m_primed = false;
};

/*!
 * One row of the history in metrics.db: a minute (span 60) or an hour (span
 * 3600), or a bucket of several of them as MetricsStore::series() adds them
 * up. Counts are totals over the row, averages are kept as sums and counts.
 */
struct MetricsRow
{
    qint64 t = 0;              //!< UTC seconds, the start of the row
    int span = 60;
    int minutes = 0;           //!< minutes in it the server was running
    qint64 requests = 0;
    qint64 clientErrors = 0;
    qint64 serverErrors = 0;
    qint64 overloaded = 0;     //!< 503: full line, too long a wait
    qint64 waitCount = 0;
    double waitSumMs = 0;
    double waitMaxMs = 0;
    qint64 workCount = 0;
    double workSumMs = 0;
    double workMaxMs = 0;
    double queueAvg = 0;
    int queueMax = 0;
    double busyAvg = 0;
    int threads = 0;
    double connectionsAvg = 0;
    int connectionsMax = 0;
    double cpuProcess = -1;
    double cpuSystem = -1;
    double memoryProcess = -1;  //!< bytes
    double memoryLoad = -1;
    qint64 bytesIn = 0;
    qint64 bytesOut = 0;
    qint64 gateCount = 0;      //!< write operations
    qint64 commits = 0;        //!< transactions they went to the disk in
    double gateWaitSumMs = 0;
    double gateWaitMaxMs = 0;

    double waitAvgMs() const { return waitCount > 0 ? waitSumMs / waitCount : 0; }
    double workAvgMs() const { return workCount > 0 ? workSumMs / workCount : 0; }
    double gateWaitAvgMs() const { return gateCount > 0 ? gateWaitSumMs / gateCount : 0; }
    /*! Operations a commit carried: above one, group commit saved fsyncs. */
    double writesPerCommit() const { return commits > 0 ? double(gateCount) / commits : 0; }
    /*! Requests a minute over the whole of the row, the minutes it was down included. */
    double requestsPerMinute() const { return span > 0 ? requests * 60.0 / span : 0; }
};

/*!
 * metrics.db next to server.db: the load of the server by minute for a week
 * and by hour for a year, and what it is right now. A file of its own, so
 * that a backup of the base does not carry it and the writes of the history
 * do not stand in the line of the base.
 *
 * The server writes it (MetricsRecorder), the admin panel only reads it.
 */
class MetricsStore
{
public:
    static constexpr int minuteRowsDays() { return 7; }
    static constexpr int hourRowsDays() { return 365; }

    static QString pathIn(const QString &dataDir);

    MetricsStore();
    ~MetricsStore();

    MetricsStore(const MetricsStore &) = delete;
    MetricsStore &operator=(const MetricsStore &) = delete;

    /*!
     * @p write: the server's connection, which creates the file and its
     * tables. The panel's does not create a missing file.
     */
    bool open(const QString &path, bool write, QString *error = nullptr);
    void close();
    bool isOpen() const;

    // ---- the server's side --------------------------------------------------
    bool writeRow(const MetricsRow &row);

    /*! Adds the minutes of the hour starting at @p hour up into its row. */
    bool rollUpHour(qint64 hour);

    /*! Forgets minutes older than a week and hours older than a year, from @p now. */
    bool prune(qint64 now);

    bool writeLive(const QJsonObject &live);

    // ---- the panel's side ---------------------------------------------------
    /*! What the server wrote last, with "at" - the UTC seconds it was written. */
    QJsonObject live();

    /*!
     * The history between @p from and @p to (UTC seconds), added up into
     * buckets of @p bucket seconds; buckets start at local midnight's
     * alignment when @p utcOffset is the offset of local time. Minute rows are
     * used while they cover the range and the bucket is under an hour,
     * hour rows otherwise. Buckets without any row are left out.
     */
    QList<MetricsRow> series(qint64 from, qint64 to, int bucket, int utcOffset = 0);

    /*! The oldest row there is, 0 when there is none. */
    qint64 firstTime();

    QString lastError() const { return m_lastError; }

private:
    QSqlDatabase m_db;
    QString m_connection;
    QString m_lastError;
};

/*!
 * Takes the measurements every few seconds on a thread of its own and keeps
 * metrics.db: a row a minute, the hour rolled up from them, the history
 * pruned to a year, and the live values every tick.
 */
class MetricsRecorder : public QObject
{
    Q_OBJECT

public:
    struct Sources
    {
        std::function<PoolSnapshot(bool takeMaxima)> pool;
        std::function<HttpSnapshot(bool takeMaxima)> http;
        std::function<GateStats()> gate;
    };

    /*! @p tickMs 0: no timer, the tests call tick() themselves. */
    MetricsRecorder(const QString &path, Sources sources, int tickMs = 5000);
    ~MetricsRecorder() override;

    bool start(QString *error = nullptr);
    void stop();

    /*! One measurement at @p now (UTC seconds), on the thread of the recorder; blocks. */
    void tick(qint64 now);

    /*! Writes the minute that is being gathered even if it is not over, for the tests. */
    void flushMinute();

    /*! Extra fields of the live values: the version, the limits. */
    void setStatic(const QJsonObject &fields) { m_static = fields; }

private:
    struct Tick
    {
        qint64 at = 0;
        qint64 requests = 0;
        qint64 waitCount = 0;
        double waitSumMs = 0;
        qint64 workCount = 0;
        double workSumMs = 0;
        qint64 overloaded = 0;
    };

    void measure(qint64 now);
    void closeMinute();
    void resetMinute(qint64 start);
    void openOnThread(QString *error, bool *ok);
    void closeOnThread();

    QString m_path;
    Sources m_sources;
    int m_tickMs;
    QJsonObject m_static;

    QThread *m_thread = nullptr;
    QObject *m_context = nullptr;   //!< lives on m_thread; the timer and the calls go through it
    QTimer *m_timer = nullptr;
    MetricsStore *m_store = nullptr;
    ResourceProbe m_probe;
    qint64 m_startedAt = 0;

    // The previous totals, to make the deltas of a tick from.
    bool m_haveLast = false;
    PoolSnapshot m_lastPool;
    HttpSnapshot m_lastHttp;
    GateStats m_lastGate;
    ResourceProbe::Sample m_lastResources;

    // The minute being gathered: its counts in m_minute, the sums of the
    // values that are averaged over its ticks here.
    MetricsRow m_minute;
    int m_samples = 0;
    double m_queueSum = 0;
    double m_busySum = 0;
    double m_connectionsSum = 0;
    double m_cpuProcessSum = 0;
    int m_cpuProcessSamples = 0;
    double m_cpuSystemSum = 0;
    int m_cpuSystemSamples = 0;
    double m_memorySum = 0;
    double m_memoryLoadSum = 0;
    int m_memorySamples = 0;
    int m_memoryLoadSamples = 0;
    qint64 m_lastPrune = 0;

    std::deque<Tick> m_recent;   //!< the last minute of ticks, for the live values
};
