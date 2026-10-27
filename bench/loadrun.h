#pragma once

#include <QJsonObject>
#include <QString>

#include <atomic>
#include <functional>

/*!
 * The load settings of one run of the server: what goes into the [server]
 * group of its server.ini (see ServerConfig).
 */
struct ServerSettings
{
    int threads = 0;
    QString dispatch = QStringLiteral("fair");
    int groupCommit = 64;
    QString syncMode = QStringLiteral("full");
    int maxConnections = 1024;
    int bufferMb = 0;
    int maxQueueWaitSeconds = 30;
    int maxBodyMb = 64;

    QByteArray ini() const;
    QJsonObject toJson() const;
    static ServerSettings fromJson(const QJsonObject &object);

    /*! "потоков 8, запись по 64, fsync" - short, for a line of progress. */
    QString describe() const;

    bool operator==(const ServerSettings &other) const;
};

/*! What the server of a run may use: cores, memory, a share of the processor. */
struct ServerLimits
{
    int affinity = 0;    //!< the first N cores only (Windows)
    int memoryMb = 0;    //!< commit no more (a job object, Windows)
    int cpuRate = 0;     //!< no more than N % of the processor (Windows)
};

/*!
 * One run: a fresh copy of the prepared base, the real server on it with
 * @c settings, @c clients clients doing @c scenario for @c seconds.
 *
 * Scenarios: pull (what is new), push (5 changes a request), mixed (70 %
 * pulls, 20 % pushes, 3 % logins, 7 % /api/me), skewed (a quarter of the
 * clients one account pushing 100 changes of 1 KB, the rest pulling), bulk
 * (400 changes of 4 KB - a whole planner a request).
 */
struct LoadOptions
{
    QString serverExe;
    QString baseDir;     //!< prepared by prepareBase()
    QString workDir;     //!< the run gets a folder of its own here
    QString scenario = QStringLiteral("mixed");
    int clients = 64;
    int seconds = 20;
    int warmupMs = 2000;
    ServerSettings settings;
    ServerLimits limits;
    bool keep = false;   //!< leave the folder of the run
};

struct LoadResult
{
    bool started = false;       //!< the server came up and answered
    QString error;
    double seconds = 0;
    qint64 requests = 0;        //!< answered while measuring
    double throughput = 0;      //!< requests a second
    double p50 = 0;
    double p95 = 0;
    double p99 = 0;
    double lightP50 = 0;        //!< skewed: the clients that only pull
    double lightP99 = 0;
    int refused = 0;            //!< 503
    int errors = 0;             //!< other failures: statuses, broken connections
    double cpuPercent = -1;     //!< of the cores the server could use
    double peakMemoryMb = -1;
    double threadSpread = -1;   //!< deviation of requests by thread, % of the mean
    double writesPerCommit = -1;
    bool alive = false;         //!< answers after the run
    bool died = false;          //!< exited during it
    QString keptFolder;

    /*! A run whose numbers can be compared: it ran, nothing failed, the server lived. */
    bool usable() const { return started && !died && alive && errors == 0 && requests > 0; }

    QJsonObject toJson() const;
    static LoadResult fromJson(const QJsonObject &object);
};

/*! @p fraction of the run done (0..1) and what is happening, called a few times a second. */
using RunProgress = std::function<void(double fraction, const QString &message)>;

/*!
 * Runs the server on a copy of the base and loads it. @p cancel stops the
 * run at once: the server is killed, the result is whatever was measured.
 */
LoadResult runLoad(const LoadOptions &options, const std::atomic<bool> *cancel,
                   const RunProgress &progress = RunProgress());

/*! A free TCP port on 127.0.0.1. */
quint16 freePort();

/*! Asks /api/ping on 127.0.0.1:@p port; true on 200 within @p timeoutMs. */
bool pingServer(quint16 port, int timeoutMs = 2000);

/*! The server next to this program: CalendarServer(.exe) in its folder. */
QString defaultServerExe();
