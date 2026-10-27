#include "basemaker.h"
#include "config.h"
#include "loadrun.h"
#include "probe.h"
#include "reporter.h"
#include "storage.h"
#include "tuner.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <mutex>

/*!
 * The benchmark without its long runs: how it chooses, what it reports, the
 * base it makes, the machine it measures, and one short run of the real
 * server it starts and stops.
 */
class BenchTest : public QObject
{
    Q_OBJECT

private slots:
    void candidatesFollowTheCores();
    void bestIsFastNotNoisy();
    void fairerQueueWinsOnLightClients();
    void settingsTravelAsIniAndJson();
    void progressIsJsonLinesWithHeartbeat();
    void baseIsMadeOnceForItsNumbers();
    void machineIsMeasured();
    void shortRunOfTheRealServer();
    void runStopsWhenCancelled();

private:
    static LoadResult result(double throughput, double p95, bool usable = true);
    QString serverExe() const;
};

LoadResult BenchTest::result(double throughput, double p95, bool usable)
{
    LoadResult one;
    one.started = true;
    one.alive = usable;
    one.requests = usable ? 100 : 0;
    one.throughput = throughput;
    one.p95 = p95;
    return one;
}

QString BenchTest::serverExe() const
{
    // The server is built next to the tests.
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("CalendarServer") + QStringLiteral(
#ifdef Q_OS_WIN
            ".exe"
#else
            ""
#endif
            ));
}

void BenchTest::candidatesFollowTheCores()
{
    QCOMPARE(Tuner::threadCandidates(4), QList<int>({ 2, 4, 8, 16 }));
    QCOMPARE(Tuner::threadCandidates(1), QList<int>({ 2, 4 }));
    QCOMPARE(Tuner::threadCandidates(32), QList<int>({ 16, 32, 64 }));
    QCOMPARE(Tuner::commitCandidates(), QList<int>({ 0, 8, 64, 256 }));
    // The current value costs nothing; a current that is no candidate - the smaller the cheaper.
    QCOMPARE(Tuner::costsAround({ 2, 4, 8, 16 }, 4), QList<int>({ 2, 0, 2, 3 }));
    QCOMPARE(Tuner::costsAround({ 2, 4, 8, 16 }, 6), QList<int>({ 1, 2, 3, 4 }));
}

/*!
 * The choice: the fastest, unless one within 5 % of it is what runs now; a
 * run with failures never; one with a far worse p95 not either.
 */
void BenchTest::bestIsFastNotNoisy()
{
    const QList<int> costs = { 2, 0, 2, 3 };
    // A clear winner wins.
    QCOMPARE(Tuner::pickBest({ result(100, 50), result(200, 50), result(300, 50), result(410, 50) }, costs),
             3);
    // Two within 5 % of each other: the cheaper of them.
    QCOMPARE(Tuner::pickBest({ result(100, 50), result(200, 50), result(400, 50), result(410, 50) }, costs),
             2);
    // Within the noise the current value stays.
    QCOMPARE(Tuner::pickBest({ result(100, 50), result(400, 50), result(410, 50), result(405, 50) }, costs),
             1);
    // Faster but with a p95 three times worse: not that one.
    QCOMPARE(Tuner::pickBest({ result(100, 50), result(390, 50), result(400, 150), result(100, 50) },
                             { 1, 1, 0, 1 }),
             1);
    // The fastest failed: the next one.
    QCOMPARE(Tuner::pickBest({ result(100, 50), result(300, 50), result(900, 50, false) }, { 0, 1, 2 }), 1);
    // Nothing usable: nothing.
    QCOMPARE(Tuner::pickBest({ result(100, 50, false) }, { 0 }), -1);
}

void BenchTest::fairerQueueWinsOnLightClients()
{
    LoadResult fair = result(200, 100);
    fair.lightP99 = 200;
    LoadResult fifo = result(20, 100);
    fifo.lightP99 = 3800;
    QCOMPARE(Tuner::pickFairer({ fair, fifo }, 0), 0);
    QCOMPARE(Tuner::pickFairer({ fair, fifo }, 1), 0);
    // A tie keeps the preferred one.
    fifo = result(195, 100);
    fifo.lightP99 = 210;
    QCOMPARE(Tuner::pickFairer({ fair, fifo }, 1), 1);
}

void BenchTest::settingsTravelAsIniAndJson()
{
    ServerSettings settings;
    settings.threads = 12;
    settings.dispatch = QStringLiteral("fifo");
    settings.groupCommit = 256;
    settings.syncMode = QStringLiteral("normal");
    settings.maxConnections = 300;
    settings.bufferMb = 128;
    settings.maxQueueWaitSeconds = 15;
    settings.maxBodyMb = 16;
    QCOMPARE(ServerSettings::fromJson(settings.toJson()), settings);

    QTemporaryDir dir;
    QFile file(dir.filePath(QStringLiteral("server.ini")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(settings.ini());
    file.close();
    ServerConfig config;
    QString error;
    QVERIFY2(config.loadIni(file.fileName(), &error), qPrintable(error));
    QCOMPARE(config.threads, 12);
    QCOMPARE(config.dispatch, QStringLiteral("fifo"));
    QCOMPARE(config.groupCommit, 256);
    QCOMPARE(config.syncMode, QStringLiteral("normal"));
    QCOMPARE(config.maxConnections, 300);
    QCOMPARE(config.bufferMb, 128);
    QCOMPARE(config.maxQueueWaitSeconds, 15);
    QCOMPARE(config.maxBodyMb, 16);
    QVERIFY(!settings.describe().isEmpty());
}

/*!
 * What the admin panel reads: a JSON object a line, each with its event,
 * and a heartbeat every second that says how long nothing moved.
 */
void BenchTest::progressIsJsonLinesWithHeartbeat()
{
    std::mutex lock;
    QList<QJsonObject> events;
    {
        Reporter reporter(true, [&](const QByteArray &line) {
            QVERIFY(line.endsWith('\n'));
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(line, &error);
            QCOMPARE(error.error, QJsonParseError::NoError);
            std::lock_guard<std::mutex> guard(lock);
            events << document.object();
        });
        reporter.plan({ { QStringLiteral("probe"), QStringLiteral("Проверка") } }, 3, 60);
        reporter.stage(0, QStringLiteral("probe"), QStringLiteral("Проверка"));
        reporter.progress(12.5, QStringLiteral("шаг"));
        QTest::qWait(2300);
    }
    QStringList names;
    double lastIdle = -1;
    for (const QJsonObject &event : events) {
        names << event.value(QStringLiteral("event")).toString();
        QVERIFY(event.contains(QStringLiteral("elapsed")));
        if (names.last() == QLatin1String("heartbeat")) {
            QCOMPARE(event.value(QStringLiteral("percent")).toDouble(), 12.5);
            QCOMPARE(event.value(QStringLiteral("message")).toString(), QStringLiteral("шаг"));
            lastIdle = event.value(QStringLiteral("idle")).toDouble();
        }
    }
    QCOMPARE(names.mid(0, 3), QStringList({ QStringLiteral("plan"), QStringLiteral("stage"),
                                            QStringLiteral("progress") }));
    QVERIFY2(names.count(QStringLiteral("heartbeat")) >= 2, qPrintable(names.join(',')));
    QVERIFY(lastIdle >= 1.5);
    QCOMPARE(events.first().value(QStringLiteral("runs")).toInt(), 3);
}

void BenchTest::baseIsMadeOnceForItsNumbers()
{
    QTemporaryDir dir;
    BaseRequest request;
    request.dir = dir.filePath(QStringLiteral("base"));
    request.accounts = 4;
    request.records = 30;
    QVERIFY(!baseIsReady(request));
    QString error;
    int steps = 0;
    QVERIFY2(prepareBase(request, nullptr, [&](double, const QString &) { ++steps; }, &error),
             qPrintable(error));
    QVERIFY(steps > 4);
    QVERIFY(baseIsReady(request));
    const DatabaseInfo info = Storage::inspect(QDir(request.dir).filePath(QStringLiteral("server.db")));
    QCOMPARE(info.users, 4);
    QVERIFY(info.records >= 4 * 30);
    // Other numbers: another base.
    request.records = 31;
    QVERIFY(!baseIsReady(request));
}

void BenchTest::machineIsMeasured()
{
    QTemporaryDir dir;
    int steps = 0;
    const SystemInfo info = probeSystem(dir.path(), nullptr, [&](double, const QString &) { ++steps; });
    QVERIFY(steps >= 4);
    QVERIFY(info.cores >= 1);
    QVERIFY(info.memoryTotal > 0);
    QVERIFY(info.hashMs > 0);
    QVERIFY(info.hashParallel > 0.5);
    QVERIFY(info.commitMs > 0);
    QVERIFY(info.commitNormalMs > 0);
    QVERIFY(info.writeMBps > 0);
    QVERIFY(info.freeBytes > 0);
    QCOMPARE(SystemInfo::fromJson(info.toJson()).commitMs, info.commitMs);
    QVERIFY(info.describe().size() >= 3);
    // The probe leaves nothing behind.
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 0);
}

/*! A short run of the real server: it starts, answers, is measured and goes away. */
void BenchTest::shortRunOfTheRealServer()
{
    if (!QFile::exists(serverExe()))
        QSKIP("CalendarServer is not built next to the tests");
    QTemporaryDir dir;
    BaseRequest request;
    request.dir = dir.filePath(QStringLiteral("base"));
    request.accounts = 4;
    request.records = 30;
    QString error;
    QVERIFY2(prepareBase(request, nullptr, {}, &error), qPrintable(error));

    LoadOptions options;
    options.serverExe = serverExe();
    options.baseDir = request.dir;
    options.workDir = dir.filePath(QStringLiteral("runs"));
    options.scenario = QStringLiteral("mixed");
    options.clients = 4;
    options.seconds = 2;
    options.warmupMs = 500;
    options.settings.threads = 2;
    double lastFraction = 0;
    const LoadResult run = runLoad(options, nullptr, [&](double fraction, const QString &) {
        lastFraction = fraction;
    });
    QVERIFY2(run.usable(), qPrintable(run.error));
    QVERIFY(run.throughput > 0);
    QVERIFY(run.p50 > 0);
    QCOMPARE(lastFraction, 1.0);
    // Shorter than a tick of the server's metrics: no spread by threads yet.
    // The folder of the run is gone with it.
    QCOMPARE(QDir(options.workDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
}

/*! Cancelling stops a long run at once and takes its server along. */
void BenchTest::runStopsWhenCancelled()
{
    if (!QFile::exists(serverExe()))
        QSKIP("CalendarServer is not built next to the tests");
    QTemporaryDir dir;
    BaseRequest request;
    request.dir = dir.filePath(QStringLiteral("base"));
    request.accounts = 4;
    request.records = 30;
    QString error;
    QVERIFY2(prepareBase(request, nullptr, {}, &error), qPrintable(error));

    LoadOptions options;
    options.serverExe = serverExe();
    options.baseDir = request.dir;
    options.workDir = dir.filePath(QStringLiteral("runs"));
    options.clients = 4;
    options.seconds = 60;
    std::atomic<bool> cancel { false };
    QElapsedTimer timer;
    timer.start();
    QThread *stopper = QThread::create([&] {
        QThread::msleep(2500);
        cancel = true;
    });
    stopper->start();
    const LoadResult run = runLoad(options, &cancel);
    stopper->wait();
    delete stopper;
    QVERIFY2(timer.elapsed() < 15000, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(run.error, QStringLiteral("прервано"));
}

QTEST_GUILESS_MAIN(BenchTest)
#include "tst_bench.moc"
