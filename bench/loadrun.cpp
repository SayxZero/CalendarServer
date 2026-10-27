#include "loadrun.h"

#include "metrics.h"
#include "storage.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace {

const QString kPassword = QStringLiteral("secret1");

struct Account
{
    QString login;
    QString token;
    int records = 0;
};

struct Sample
{
    double ms = 0;
    int status = 0;
    bool light = true;
};

/*! One client: a connection of its own on a thread of its own, blocking. */
class Client
{
public:
    Client(quint16 port, const Account &account, int index, const QString &scenario,
           const std::atomic<bool> *stop, const std::atomic<bool> *measuring,
           std::atomic<qint64> *answered)
        : m_port(port)
        , m_account(account)
        , m_index(index)
        , m_scenario(scenario)
        , m_stop(stop)
        , m_measuring(measuring)
        , m_answered(answered)
        , m_random(quint32(index * 7919 + 13))
    {
        // In "skewed" a quarter of the clients belong to one heavy account
        // that pushes big batches; the rest only ask what is new.
        m_heavy = scenario == QLatin1String("skewed") && index % 4 == 0;
    }

    void run()
    {
        QTcpSocket socket;
        m_socket = &socket;
        if (!connectSocket())
            return;
        // Warm up: what a device does first, outside the measurement.
        exchange(pullRequest(), nullptr);
        while (!m_stop->load()) {
            const QByteArray request = next();
            QElapsedTimer timer;
            timer.start();
            int status = 0;
            if (!exchange(request, &status)) {
                if (m_stop->load())
                    break;
                if (m_measuring->load())
                    ++failures;
                if (!connectSocket())
                    QThread::msleep(100);
                continue;
            }
            if (m_measuring->load()) {
                samples.push_back({ double(timer.nsecsElapsed()) / 1e6, status, !m_heavy });
                ++*m_answered;
            }
            if (status == 503)
                QThread::msleep(50);
        }
        socket.disconnectFromHost();
    }

    std::vector<Sample> samples;
    int failures = 0;

private:
    bool connectSocket()
    {
        m_socket->abort();
        m_socket->connectToHost(QStringLiteral("127.0.0.1"), m_port);
        return m_socket->waitForConnected(30000);
    }

    QByteArray http(const QByteArray &method, const QByteArray &path, const QByteArray &payload,
                    bool auth = true)
    {
        QByteArray request = method + ' ' + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
        if (auth)
            request += "Authorization: Bearer " + m_account.token.toLatin1() + "\r\n";
        request += "Content-Type: application/json\r\nContent-Length: "
                   + QByteArray::number(payload.size()) + "\r\n\r\n" + payload;
        return request;
    }

    QByteArray pullRequest()
    {
        QJsonObject body;
        body.insert(QStringLiteral("since"), double(m_cursor));
        body.insert(QStringLiteral("device"), QStringLiteral("bench-%1").arg(m_index));
        body.insert(QStringLiteral("changes"), QJsonArray());
        return http("POST", "/api/sync", QJsonDocument(body).toJson(QJsonDocument::Compact));
    }

    QByteArray pushRequest(int count, int notes)
    {
        QJsonArray changes;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (int i = 0; i < count; ++i) {
            const QString id = QStringLiteral("e%1").arg(m_random.bounded(qMax(1, m_account.records)));
            QJsonObject data;
            data.insert(QStringLiteral("id"), id);
            data.insert(QStringLiteral("calendarId"), QStringLiteral("c%1").arg(m_random.bounded(3)));
            data.insert(QStringLiteral("title"), QStringLiteral("Правка %1").arg(now));
            if (notes > 0)
                data.insert(QStringLiteral("notes"), QString(notes, QLatin1Char('n')));
            QJsonObject change;
            change.insert(QStringLiteral("kind"), QStringLiteral("event"));
            change.insert(QStringLiteral("id"), id);
            change.insert(QStringLiteral("modified"), double(now));
            change.insert(QStringLiteral("data"), data);
            changes.append(change);
        }
        QJsonObject body;
        body.insert(QStringLiteral("since"), double(m_cursor));
        body.insert(QStringLiteral("device"), QStringLiteral("bench-%1").arg(m_index));
        body.insert(QStringLiteral("changes"), changes);
        return http("POST", "/api/sync", QJsonDocument(body).toJson(QJsonDocument::Compact));
    }

    QByteArray loginRequest()
    {
        QJsonObject body;
        body.insert(QStringLiteral("login"), m_account.login);
        body.insert(QStringLiteral("password"), kPassword);
        body.insert(QStringLiteral("device"), QStringLiteral("bench"));
        return http("POST", "/api/login", QJsonDocument(body).toJson(QJsonDocument::Compact), false);
    }

    QByteArray next()
    {
        if (m_scenario == QLatin1String("pull"))
            return pullRequest();
        if (m_scenario == QLatin1String("push"))
            return pushRequest(5, 0);
        if (m_scenario == QLatin1String("skewed"))
            return m_heavy ? pushRequest(100, 1024) : pullRequest();
        if (m_scenario == QLatin1String("bulk"))
            return pushRequest(400, 4096);   // a whole planner at once: ~1.7 MB
        // mixed: what a crowd of planners looks like.
        const int roll = int(m_random.bounded(100));
        if (roll < 70)
            return pullRequest();
        if (roll < 90)
            return pushRequest(5, 0);
        if (roll < 93)
            return loginRequest();
        return http("GET", "/api/me", QByteArray());
    }

    bool exchange(const QByteArray &request, int *status)
    {
        if (m_socket->state() != QAbstractSocket::ConnectedState && !connectSocket())
            return false;
        m_socket->write(request);
        if (!m_socket->waitForBytesWritten(60000))
            return false;
        QByteArray buffer;
        int headEnd = -1;
        qint64 length = -1;
        while (true) {
            if (headEnd < 0) {
                headEnd = buffer.indexOf("\r\n\r\n");
                if (headEnd >= 0) {
                    for (const QByteArray &line : buffer.left(headEnd).split('\n')) {
                        const QByteArray lower = line.trimmed().toLower();
                        if (lower.startsWith("content-length:"))
                            length = lower.mid(15).trimmed().toLongLong();
                    }
                }
            }
            if (headEnd >= 0 && length >= 0 && buffer.size() >= headEnd + 4 + length)
                break;
            // Waits in slices, so that a stopped run does not wait for an answer.
            if (!m_socket->waitForReadyRead(250)) {
                if (m_stop->load() || m_socket->state() != QAbstractSocket::ConnectedState)
                    return false;
                continue;
            }
            buffer += m_socket->readAll();
        }
        const int code = buffer.mid(9, 3).toInt();
        if (status)
            *status = code;
        if (code == 200 || code == 201) {
            const QJsonObject answer =
                QJsonDocument::fromJson(buffer.mid(headEnd + 4, length)).object();
            if (answer.contains(QStringLiteral("revision")))
                m_cursor = qint64(answer.value(QStringLiteral("revision")).toDouble());
        }
        // The server closes after an error of the protocol; open a new one.
        if (buffer.left(headEnd).toLower().contains("connection: close"))
            m_socket->disconnectFromHost();
        return true;
    }

    QTcpSocket *m_socket = nullptr;
    quint16 m_port;
    Account m_account;
    int m_index;
    QString m_scenario;
    const std::atomic<bool> *m_stop;
    const std::atomic<bool> *m_measuring;
    std::atomic<qint64> *m_answered;
    QRandomGenerator m_random;
    qint64 m_cursor = 0;
    bool m_heavy = false;
};

double percentile(std::vector<double> values, double p)
{
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    const size_t at = size_t(std::min<double>(values.size() - 1, std::ceil(p * values.size()) - 1));
    return values[at];
}

#ifdef Q_OS_WIN
/*!
 * The job the server of a run lives in: its limits, its accounting, and its
 * end - closing the job kills it, so a benchmark that is stopped or dies
 * takes its server along.
 */
struct Job
{
    HANDLE handle = nullptr;

    bool create(const ServerLimits &limits)
    {
        handle = CreateJobObjectW(nullptr, nullptr);
        if (!handle)
            return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (limits.memoryMb > 0) {
            info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            info.ProcessMemoryLimit = SIZE_T(limits.memoryMb) * 1024 * 1024;
        }
        if (!SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &info, sizeof(info)))
            return false;
        if (limits.cpuRate > 0) {
            JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate = {};
            rate.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
            rate.CpuRate = DWORD(limits.cpuRate * 100);
            if (!SetInformationJobObject(handle, JobObjectCpuRateControlInformation, &rate, sizeof(rate)))
                return false;
        }
        return true;
    }

    /*!
     * Puts the suspended process in and lets it go: started suspended, it
     * has run no code yet, and it learns its limits (ServerConfig::memoryLimit())
     * like a process started in a job.
     */
    bool adopt(qint64 pid, int affinity)
    {
        HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, DWORD(pid));
        if (!process)
            return false;
        if (affinity > 0)
            SetProcessAffinityMask(process, (DWORD_PTR(1) << qMin(affinity, 63)) - 1);
        const bool ok = AssignProcessToJobObject(handle, process);
        CloseHandle(process);
        HANDLE threads = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (threads != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry;
            entry.dwSize = sizeof(entry);
            for (BOOL more = Thread32First(threads, &entry); more; more = Thread32Next(threads, &entry)) {
                if (entry.th32OwnerProcessID != DWORD(pid))
                    continue;
                if (HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID)) {
                    ResumeThread(thread);
                    CloseHandle(thread);
                }
            }
            CloseHandle(threads);
        }
        return ok;
    }

    void accounting(double *cpuSeconds, double *peakMb) const
    {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION basic = {};
        if (QueryInformationJobObject(handle, JobObjectBasicAccountingInformation, &basic,
                                      sizeof(basic), nullptr)) {
            *cpuSeconds = double(basic.TotalUserTime.QuadPart + basic.TotalKernelTime.QuadPart) / 1e7;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION extended = {};
        if (QueryInformationJobObject(handle, JobObjectExtendedLimitInformation, &extended,
                                      sizeof(extended), nullptr)) {
            *peakMb = double(extended.PeakProcessMemoryUsed) / 1024 / 1024;
        }
    }

    ~Job()
    {
        if (handle)
            CloseHandle(handle);
    }
};
#endif

QList<Account> readAccounts(const QString &baseDir, QString *error)
{
    QList<Account> accounts;
    QFile file(QDir(baseDir).filePath(QStringLiteral("tokens.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("в %1 нет tokens.json: сначала prepare").arg(QDir::toNativeSeparators(baseDir));
        return accounts;
    }
    for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).array()) {
        const QJsonObject entry = value.toObject();
        accounts.append({ entry.value(QStringLiteral("login")).toString(),
                          entry.value(QStringLiteral("token")).toString(),
                          entry.value(QStringLiteral("records")).toInt() });
    }
    if (accounts.isEmpty())
        *error = QStringLiteral("в tokens.json нет аккаунтов");
    return accounts;
}

} // namespace

// ------------------------------------------------------------------- settings

QByteArray ServerSettings::ini() const
{
    QByteArray text = "[server]\n";
    text += "threads=" + QByteArray::number(threads) + '\n';
    text += "dispatch=" + dispatch.toUtf8() + '\n';
    text += "group_commit=" + QByteArray::number(groupCommit) + '\n';
    text += "sync_mode=" + syncMode.toUtf8() + '\n';
    text += "max_connections=" + QByteArray::number(maxConnections) + '\n';
    text += "buffer_mb=" + QByteArray::number(bufferMb) + '\n';
    text += "max_queue_wait_seconds=" + QByteArray::number(maxQueueWaitSeconds) + '\n';
    text += "max_body_mb=" + QByteArray::number(maxBodyMb) + '\n';
    return text;
}

QJsonObject ServerSettings::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("threads"), threads);
    object.insert(QStringLiteral("dispatch"), dispatch);
    object.insert(QStringLiteral("group_commit"), groupCommit);
    object.insert(QStringLiteral("sync_mode"), syncMode);
    object.insert(QStringLiteral("max_connections"), maxConnections);
    object.insert(QStringLiteral("buffer_mb"), bufferMb);
    object.insert(QStringLiteral("max_queue_wait_seconds"), maxQueueWaitSeconds);
    object.insert(QStringLiteral("max_body_mb"), maxBodyMb);
    return object;
}

ServerSettings ServerSettings::fromJson(const QJsonObject &object)
{
    ServerSettings settings;
    settings.threads = object.value(QStringLiteral("threads")).toInt(settings.threads);
    settings.dispatch = object.value(QStringLiteral("dispatch")).toString(settings.dispatch);
    settings.groupCommit = object.value(QStringLiteral("group_commit")).toInt(settings.groupCommit);
    settings.syncMode = object.value(QStringLiteral("sync_mode")).toString(settings.syncMode);
    settings.maxConnections = object.value(QStringLiteral("max_connections")).toInt(settings.maxConnections);
    settings.bufferMb = object.value(QStringLiteral("buffer_mb")).toInt(settings.bufferMb);
    settings.maxQueueWaitSeconds =
        object.value(QStringLiteral("max_queue_wait_seconds")).toInt(settings.maxQueueWaitSeconds);
    settings.maxBodyMb = object.value(QStringLiteral("max_body_mb")).toInt(settings.maxBodyMb);
    return settings;
}

QString ServerSettings::describe() const
{
    return QStringLiteral("потоков %1, очередь %2, запись %3, %4")
        .arg(threads > 0 ? QString::number(threads) : QStringLiteral("по ядрам"), dispatch,
             groupCommit > 0 ? QStringLiteral("по %1").arg(groupCommit) : QStringLiteral("по одной"),
             syncMode == QLatin1String("normal") ? QStringLiteral("без fsync") : QStringLiteral("fsync"));
}

bool ServerSettings::operator==(const ServerSettings &other) const
{
    return toJson() == other.toJson();
}

QJsonObject LoadResult::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("started"), started);
    if (!error.isEmpty())
        object.insert(QStringLiteral("error"), error);
    object.insert(QStringLiteral("seconds"), seconds);
    object.insert(QStringLiteral("requests"), double(requests));
    object.insert(QStringLiteral("throughput"), throughput);
    object.insert(QStringLiteral("p50"), p50);
    object.insert(QStringLiteral("p95"), p95);
    object.insert(QStringLiteral("p99"), p99);
    object.insert(QStringLiteral("lightP50"), lightP50);
    object.insert(QStringLiteral("lightP99"), lightP99);
    object.insert(QStringLiteral("refused"), refused);
    object.insert(QStringLiteral("errors"), errors);
    object.insert(QStringLiteral("cpuPercent"), cpuPercent);
    object.insert(QStringLiteral("peakMemoryMb"), peakMemoryMb);
    object.insert(QStringLiteral("threadSpread"), threadSpread);
    object.insert(QStringLiteral("writesPerCommit"), writesPerCommit);
    object.insert(QStringLiteral("alive"), alive);
    object.insert(QStringLiteral("died"), died);
    if (!keptFolder.isEmpty())
        object.insert(QStringLiteral("keptFolder"), keptFolder);
    return object;
}

LoadResult LoadResult::fromJson(const QJsonObject &object)
{
    LoadResult result;
    result.started = object.value(QStringLiteral("started")).toBool();
    result.error = object.value(QStringLiteral("error")).toString();
    result.seconds = object.value(QStringLiteral("seconds")).toDouble();
    result.requests = qint64(object.value(QStringLiteral("requests")).toDouble());
    result.throughput = object.value(QStringLiteral("throughput")).toDouble();
    result.p50 = object.value(QStringLiteral("p50")).toDouble();
    result.p95 = object.value(QStringLiteral("p95")).toDouble();
    result.p99 = object.value(QStringLiteral("p99")).toDouble();
    result.lightP50 = object.value(QStringLiteral("lightP50")).toDouble();
    result.lightP99 = object.value(QStringLiteral("lightP99")).toDouble();
    result.refused = object.value(QStringLiteral("refused")).toInt();
    result.errors = object.value(QStringLiteral("errors")).toInt();
    result.cpuPercent = object.value(QStringLiteral("cpuPercent")).toDouble(-1);
    result.peakMemoryMb = object.value(QStringLiteral("peakMemoryMb")).toDouble(-1);
    result.threadSpread = object.value(QStringLiteral("threadSpread")).toDouble(-1);
    result.writesPerCommit = object.value(QStringLiteral("writesPerCommit")).toDouble(-1);
    result.alive = object.value(QStringLiteral("alive")).toBool();
    result.died = object.value(QStringLiteral("died")).toBool();
    result.keptFolder = object.value(QStringLiteral("keptFolder")).toString();
    return result;
}

// ---------------------------------------------------------------------- tools

quint16 freePort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0))
        return 0;
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

bool pingServer(quint16 port, int timeoutMs)
{
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), port);
    if (!socket.waitForConnected(qMin(timeoutMs, 1000)))
        return false;
    socket.write("GET /api/ping HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    if (!socket.waitForReadyRead(timeoutMs))
        return false;
    return socket.readAll().startsWith("HTTP/1.1 200");
}

QString defaultServerExe()
{
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("CalendarServer.exe");
#else
    const QString name = QStringLiteral("CalendarServer");
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

// ------------------------------------------------------------------------ run

LoadResult runLoad(const LoadOptions &options, const std::atomic<bool> *cancel,
                   const RunProgress &progress)
{
    LoadResult result;
    const auto cancelled = [cancel] { return cancel && cancel->load(); };
    const auto say = [&](double fraction, const QString &message) {
        if (progress)
            progress(qBound(0.0, fraction, 1.0), message);
    };

    QString error;
    const QList<Account> accounts = readAccounts(options.baseDir, &error);
    if (accounts.isEmpty()) {
        result.error = error;
        return result;
    }

    // A fresh copy of the base: every run starts from the same data.
    QDir().mkpath(options.workDir);
    QTemporaryDir work(QDir(options.workDir).filePath(QStringLiteral("run-XXXXXX")));
    work.setAutoRemove(!options.keep);
    if (!work.isValid()) {
        result.error = QStringLiteral("не удалось создать папку прогона в %1")
                           .arg(QDir::toNativeSeparators(options.workDir));
        return result;
    }
    say(0, QStringLiteral("копия базы"));
    if (!Storage::copyDatabase(QDir(options.baseDir).filePath(QStringLiteral("server.db")),
                               QDir(work.path()).filePath(QStringLiteral("server.db")), &error)) {
        result.error = QStringLiteral("копия базы: %1").arg(error);
        return result;
    }
    QFile ini(QDir(work.path()).filePath(QStringLiteral("server.ini")));
    if (!ini.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("не записан server.ini прогона");
        return result;
    }
    ini.write(options.settings.ini());
    ini.close();

    const quint16 port = freePort();
    QProcess process;
    process.setProgram(options.serverExe);
    process.setArguments({ QStringLiteral("--host"), QStringLiteral("127.0.0.1"), QStringLiteral("-p"),
                           QString::number(port), QStringLiteral("-d"), work.path(),
                           QStringLiteral("--config"), ini.fileName(), QStringLiteral("--log-file"),
                           QDir(work.path()).filePath(QStringLiteral("server.log")) });
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QDir(work.path()).filePath(QStringLiteral("stderr.txt")));
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_SUSPENDED;
    });
    Job job;
    if (!job.create(options.limits)) {
        result.error = QStringLiteral("объект задания: ошибка %1").arg(GetLastError());
        return result;
    }
#endif
    say(0, QStringLiteral("запуск сервера"));
    process.start();
    if (!process.waitForStarted(10000)) {
        result.error = QStringLiteral("сервер не запустился: %1").arg(process.errorString());
        return result;
    }
#ifdef Q_OS_WIN
    if (!job.adopt(process.processId(), options.limits.affinity)) {
        result.error = QStringLiteral("объект задания: ошибка %1").arg(GetLastError());
        process.kill();
        return result;
    }
#endif
    QElapsedTimer startTimer;
    startTimer.start();
    while (!pingServer(port, 1000)) {
        if (cancelled() || startTimer.elapsed() > 60000 || process.state() == QProcess::NotRunning) {
            QFile stderrFile(QDir(work.path()).filePath(QStringLiteral("stderr.txt")));
            stderrFile.open(QIODevice::ReadOnly);
            result.error = cancelled() ? QStringLiteral("прервано")
                                       : QStringLiteral("сервер не отвечает: %1")
                                             .arg(QString::fromUtf8(stderrFile.readAll().right(500)).trimmed());
            process.kill();
            process.waitForFinished(5000);
            return result;
        }
        QThread::msleep(100);
    }
    result.started = true;

    // ---- the clients -----------------------------------------------------------
    std::atomic<bool> stop { false };
    std::atomic<bool> measuring { false };
    std::atomic<qint64> answered { 0 };
    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < options.clients; ++i) {
        // In "skewed" all heavy clients push into one account.
        const bool heavy = options.scenario == QLatin1String("skewed") && i % 4 == 0;
        const Account &account = heavy ? accounts.first() : accounts.at(i % accounts.size());
        clients.push_back(std::make_unique<Client>(port, account, i, options.scenario, &stop,
                                                   &measuring, &answered));
    }
    std::vector<std::thread> threads;
    for (auto &client : clients)
        threads.emplace_back([&client] { client->run(); });

    const qint64 total = qint64(options.warmupMs) + qint64(options.seconds) * 1000;
    QElapsedTimer clock;
    clock.start();
    say(0, QStringLiteral("прогрев"));
    while (clock.elapsed() < options.warmupMs && !cancelled())
        QThread::msleep(50);

#ifdef Q_OS_WIN
    double cpuBefore = 0;
    double peakIgnored = 0;
    job.accounting(&cpuBefore, &peakIgnored);
#endif
    measuring = true;
    QElapsedTimer measured;
    measured.start();
    qint64 lastSay = -1000;
    while (measured.elapsed() < qint64(options.seconds) * 1000 && !cancelled()) {
        QThread::msleep(50);
        if (process.state() == QProcess::NotRunning) {
            result.died = true;
            break;
        }
        if (measured.elapsed() - lastSay >= 500) {
            lastSay = measured.elapsed();
            const double seconds = double(measured.elapsed()) / 1000;
            say(double(clock.elapsed()) / double(total),
                QStringLiteral("%1 из %2 с, %3 запросов/с")
                    .arg(qRound(seconds))
                    .arg(options.seconds)
                    .arg(seconds > 0 ? qRound(double(answered.load()) / seconds) : 0));
        }
    }
    measuring = false;
    result.seconds = double(measured.elapsed()) / 1000;
#ifdef Q_OS_WIN
    double cpuAfter = 0;
    double peakMb = 0;
    job.accounting(&cpuAfter, &peakMb);
    const int cores = qMax(1, QThread::idealThreadCount());
    const int usable = options.limits.affinity > 0 ? qMin(options.limits.affinity, cores) : cores;
    if (result.seconds > 0)
        result.cpuPercent = 100 * (cpuAfter - cpuBefore) / result.seconds / usable;
    result.peakMemoryMb = peakMb;
#endif
    stop = true;
    for (std::thread &thread : threads)
        thread.join();

    // Survived: the clients are gone, does the server still answer?
    for (int attempt = 0; attempt < 20 && !result.alive && !cancelled()
                          && process.state() != QProcess::NotRunning;
         ++attempt) {
        result.alive = pingServer(port, 2000);
        if (!result.alive)
            QThread::msleep(250);
    }
    if (process.state() == QProcess::NotRunning)
        result.died = true;
    process.kill();
    process.waitForFinished(10000);

    // ---- the numbers -------------------------------------------------------------
    std::vector<double> all;
    std::vector<double> light;
    for (const auto &client : clients) {
        result.errors += client->failures;
        for (const Sample &sample : client->samples) {
            all.push_back(sample.ms);
            if (sample.light)
                light.push_back(sample.ms);
            if (sample.status == 503)
                ++result.refused;
            else if (sample.status < 200 || sample.status >= 300)
                ++result.errors;
        }
    }
    result.requests = qint64(all.size());
    result.throughput = result.seconds > 0 ? double(all.size()) / result.seconds : 0;
    result.p50 = percentile(all, 0.5);
    result.p95 = percentile(all, 0.95);
    result.p99 = percentile(all, 0.99);
    result.lightP50 = percentile(light, 0.5);
    result.lightP99 = percentile(light, 0.99);

    MetricsStore metrics;
    if (metrics.open(MetricsStore::pathIn(work.path()), false)) {
        const QJsonObject live = metrics.live();
        const QJsonArray done = live.value(QStringLiteral("doneByThread")).toArray();
        double sum = 0;
        double sumSquares = 0;
        for (const QJsonValue &value : done) {
            sum += value.toDouble();
            sumSquares += value.toDouble() * value.toDouble();
        }
        if (!done.isEmpty() && sum > 0) {
            const double mean = sum / done.size();
            result.threadSpread =
                100 * std::sqrt(qMax(0.0, sumSquares / done.size() - mean * mean)) / mean;
        }
        if (live.contains(QStringLiteral("writesPerCommit")))
            result.writesPerCommit = live.value(QStringLiteral("writesPerCommit")).toDouble();
        metrics.close();
    }
    if (result.died) {
        QFile stderrFile(QDir(work.path()).filePath(QStringLiteral("stderr.txt")));
        if (stderrFile.open(QIODevice::ReadOnly))
            result.error = QString::fromUtf8(stderrFile.readAll().right(500)).trimmed();
        if (result.error.isEmpty())
            result.error = QStringLiteral("сервер завершился во время прогона (код %1)").arg(process.exitCode());
    }
    if (cancelled() && result.error.isEmpty())
        result.error = QStringLiteral("прервано");
    if (options.keep)
        result.keptFolder = work.path();
    say(1, QStringLiteral("%1 запросов/с").arg(qRound(result.throughput)));
    return result;
}
