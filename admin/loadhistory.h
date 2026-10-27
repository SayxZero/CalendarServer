#pragma once

#include "metrics.h"

#include <QDateTime>
#include <QList>
#include <QString>

/*! The spans the page "Нагрузка" shows the history over. */
enum class LoadPeriod { Hour, Day, Week, Month, Year };

/*!
 * One point of a chart of the load: a bucket of time, ten minutes of a day
 * or a day of a year. A bucket the server did not run in is not present, and
 * its values are NaN - a gap in the line, not a zero.
 */
struct LoadPoint
{
    QDateTime start;          //!< local time
    int seconds = 60;
    bool present = false;
    int minutes = 0;          //!< minutes of it the server ran

    qint64 requests = 0;
    qint64 overloaded = 0;
    double requestsPerMinute = 0;   //!< while it ran
    double overloadedPerMinute = 0;
    double serverErrorsPerMinute = 0;
    double queueAvg = 0;
    double queueMax = 0;
    double waitAvgMs = 0;
    double waitMaxMs = 0;
    double workAvgMs = 0;
    double workMaxMs = 0;
    double busyAvg = 0;
    double threads = 0;
    double connectionsAvg = 0;
    double connectionsMax = 0;
    double cpuProcess = 0;     //!< %, NaN when unknown
    double cpuSystem = 0;
    double memoryMb = 0;
    double memoryLoad = 0;
    double gateWaitAvgMs = 0;
    double writesPerCommit = 0;   //!< NaN where nothing was written
};

/*! What the server wrote last: the load right now. */
struct LoadNow
{
    bool known = false;       //!< metrics.db has it at all
    bool running = false;     //!< and it is fresh: written seconds ago by a running server
    QDateTime at;
    QDateTime startedAt;
    QString version;
    QString mode;
    int threads = 0;
    int busy = 0;
    int queued = 0;
    int queuedMax = 0;
    int connections = 0;
    int connectionLimit = 0;
    bool paused = false;
    double requestsPerMinute = 0;
    double waitAvgMs = 0;
    double waitMaxMs = 0;
    double workAvgMs = 0;
    double workMaxMs = 0;
    double cpuProcess = -1;
    double cpuSystem = -1;
    qint64 memoryProcess = -1;
    qint64 memoryTotal = -1;
    double memoryLoad = -1;
    double gateWaitAvgMs = 0;
    double writesPerCommit = 0;
    qint64 requestsTotal = 0;
    qint64 overloadedTotal = 0;
    qint64 buffered = 0;
    qint64 bufferBudget = 0;
    QList<qint64> doneByThread;
    QList<double> busyMsByThread;
};

/*!
 * The load of the server as the admin panel reads it from metrics.db: the
 * values of now and the history over a period, bucketed the way the chart of
 * that period draws it. No window involved: tst_admin checks it.
 */
class LoadHistory
{
public:
    /*! How old the live values may be and still mean the server runs. */
    static constexpr int freshSeconds() { return 20; }

    static int periodSeconds(LoadPeriod period);
    static int bucketSeconds(LoadPeriod period);

    bool open(const QString &dataDir, QString *error = nullptr);
    void close();
    bool isOpen() const { return m_store.isOpen(); }

    LoadNow now(const QDateTime &at = QDateTime::currentDateTime());

    /*!
     * Every bucket of the period up to @p at, oldest first, the ones the
     * server did not run in included (not present).
     */
    QList<LoadPoint> points(LoadPeriod period, const QDateTime &at = QDateTime::currentDateTime());

    /*! The oldest moment the history knows; invalid when it is empty. */
    QDateTime firstRecorded();

private:
    MetricsStore m_store;
};
