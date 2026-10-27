#include "loadhistory.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>

#include <cmath>
#include <limits>

namespace {

const double kGap = std::numeric_limits<double>::quiet_NaN();

double known(double value)
{
    return value < 0 ? kGap : value;
}

} // namespace

int LoadHistory::periodSeconds(LoadPeriod period)
{
    switch (period) {
    case LoadPeriod::Hour: return 3600;
    case LoadPeriod::Day: return 86400;
    case LoadPeriod::Week: return 7 * 86400;
    case LoadPeriod::Month: return 30 * 86400;
    case LoadPeriod::Year: return 365 * 86400;
    }
    return 3600;
}

int LoadHistory::bucketSeconds(LoadPeriod period)
{
    // About a hundred to four hundred points a chart: enough to see the
    // shape, few enough for a line that can be read.
    switch (period) {
    case LoadPeriod::Hour: return 60;
    case LoadPeriod::Day: return 600;
    case LoadPeriod::Week: return 3600;
    case LoadPeriod::Month: return 4 * 3600;
    case LoadPeriod::Year: return 86400;
    }
    return 60;
}

bool LoadHistory::open(const QString &dataDir, QString *error)
{
    return m_store.open(MetricsStore::pathIn(dataDir), false, error);
}

void LoadHistory::close()
{
    m_store.close();
}

LoadNow LoadHistory::now(const QDateTime &at)
{
    LoadNow load;
    const QJsonObject live = m_store.live();
    if (live.isEmpty())
        return load;
    load.known = true;
    load.at = QDateTime::fromSecsSinceEpoch(qint64(live.value(QStringLiteral("at")).toDouble()));
    load.running = !live.value(QStringLiteral("stopped")).toBool(false)
                   && qAbs(load.at.secsTo(at)) <= freshSeconds();
    load.startedAt =
        QDateTime::fromSecsSinceEpoch(qint64(live.value(QStringLiteral("startedAt")).toDouble()));
    load.version = live.value(QStringLiteral("version")).toString();
    load.mode = live.value(QStringLiteral("mode")).toString();
    load.threads = live.value(QStringLiteral("threads")).toInt();
    load.busy = live.value(QStringLiteral("busy")).toInt();
    load.queued = live.value(QStringLiteral("queued")).toInt();
    load.queuedMax = live.value(QStringLiteral("queuedMax")).toInt();
    load.connections = live.value(QStringLiteral("connections")).toInt();
    load.connectionLimit = live.value(QStringLiteral("connectionLimit")).toInt();
    load.paused = live.value(QStringLiteral("paused")).toBool();
    load.requestsPerMinute = live.value(QStringLiteral("requestsPerMinute")).toDouble();
    load.waitAvgMs = live.value(QStringLiteral("waitAvgMs")).toDouble();
    load.waitMaxMs = live.value(QStringLiteral("waitMaxMs")).toDouble();
    load.workAvgMs = live.value(QStringLiteral("workAvgMs")).toDouble();
    load.workMaxMs = live.value(QStringLiteral("workMaxMs")).toDouble();
    load.cpuProcess = live.value(QStringLiteral("cpuProcess")).toDouble(-1);
    load.cpuSystem = live.value(QStringLiteral("cpuSystem")).toDouble(-1);
    load.memoryProcess = qint64(live.value(QStringLiteral("memoryProcess")).toDouble(-1));
    load.memoryTotal = qint64(live.value(QStringLiteral("memoryTotal")).toDouble(-1));
    load.memoryLoad = live.value(QStringLiteral("memoryLoad")).toDouble(-1);
    load.gateWaitAvgMs = live.value(QStringLiteral("gateWaitAvgMs")).toDouble();
    load.writesPerCommit = live.value(QStringLiteral("writesPerCommit")).toDouble();
    load.requestsTotal = qint64(live.value(QStringLiteral("requestsTotal")).toDouble());
    load.overloadedTotal = qint64(live.value(QStringLiteral("overloadedTotal")).toDouble());
    load.buffered = qint64(live.value(QStringLiteral("buffered")).toDouble());
    load.bufferBudget = qint64(live.value(QStringLiteral("bufferBudget")).toDouble());
    const QJsonArray done = live.value(QStringLiteral("doneByThread")).toArray();
    const QJsonArray busy = live.value(QStringLiteral("busyMsByThread")).toArray();
    for (int i = 0; i < done.size(); ++i) {
        load.doneByThread.append(qint64(done.at(i).toDouble()));
        load.busyMsByThread.append(busy.at(i).toDouble());
    }
    return load;
}

QList<LoadPoint> LoadHistory::points(LoadPeriod period, const QDateTime &at)
{
    const int bucket = bucketSeconds(period);
    const qint64 span = periodSeconds(period);
    const int offset = at.offsetFromUtc();
    const qint64 now = at.toSecsSinceEpoch();
    // The buckets start at the local clock's round times: the days of a year
    // at local midnight, the ten minutes of a day at :00, :10...
    const auto align = [&](qint64 t) {
        return qint64(std::floor(double(t + offset) / bucket)) * bucket - offset;
    };
    const qint64 last = align(now);
    const qint64 first = last - (span / bucket - 1) * bucket;

    QHash<qint64, MetricsRow> rows;
    for (const MetricsRow &row : m_store.series(first, last + bucket, bucket, offset))
        rows.insert(row.t, row);

    QList<LoadPoint> points;
    for (qint64 t = first; t <= last; t += bucket) {
        LoadPoint point;
        point.start = QDateTime::fromSecsSinceEpoch(t);
        point.seconds = bucket;
        const auto it = rows.constFind(t);
        point.present = it != rows.constEnd() && it->minutes > 0;
        if (!point.present) {
            for (double *value :
                 { &point.requestsPerMinute, &point.overloadedPerMinute, &point.serverErrorsPerMinute,
                   &point.queueAvg, &point.queueMax, &point.waitAvgMs, &point.waitMaxMs,
                   &point.workAvgMs, &point.workMaxMs, &point.busyAvg, &point.threads,
                   &point.connectionsAvg, &point.connectionsMax, &point.cpuProcess,
                   &point.cpuSystem, &point.memoryMb, &point.memoryLoad, &point.gateWaitAvgMs,
                   &point.writesPerCommit }) {
                *value = kGap;
            }
            points.append(point);
            continue;
        }
        const MetricsRow &row = *it;
        const double minutes = qMax(1, row.minutes);
        point.minutes = row.minutes;
        point.requests = row.requests;
        point.overloaded = row.overloaded;
        point.requestsPerMinute = row.requests / minutes;
        point.overloadedPerMinute = row.overloaded / minutes;
        point.serverErrorsPerMinute = row.serverErrors / minutes;
        point.queueAvg = row.queueAvg;
        point.queueMax = row.queueMax;
        point.waitAvgMs = row.waitAvgMs();
        point.waitMaxMs = row.waitMaxMs;
        point.workAvgMs = row.workAvgMs();
        point.workMaxMs = row.workMaxMs;
        point.busyAvg = row.busyAvg;
        point.threads = row.threads;
        point.connectionsAvg = row.connectionsAvg;
        point.connectionsMax = row.connectionsMax;
        point.cpuProcess = known(row.cpuProcess);
        point.cpuSystem = known(row.cpuSystem);
        point.memoryMb = row.memoryProcess < 0 ? kGap : row.memoryProcess / (1024.0 * 1024.0);
        point.memoryLoad = known(row.memoryLoad);
        point.gateWaitAvgMs = row.gateWaitAvgMs();
        point.writesPerCommit = row.commits > 0 ? row.writesPerCommit() : kGap;
        points.append(point);
    }
    return points;
}

QDateTime LoadHistory::firstRecorded()
{
    const qint64 first = m_store.firstTime();
    return first > 0 ? QDateTime::fromSecsSinceEpoch(first) : QDateTime();
}
