#include "api.h"
#include "config.h"
#include "httpserver.h"
#include "runtimefile.h"
#include "service.h"
#include "storage.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QTextStream>
#include <QTimer>

#include <csignal>
#include <cstdio>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QTextStream &err()
{
    static QTextStream stream(stderr);
    return stream;
}

/*! Ctrl+C in a console, SIGTERM from systemd: finish the event loop cleanly. */
void requestQuit(int)
{
    QMetaObject::invokeMethod(QCoreApplication::instance(), "quit", Qt::QueuedConnection);
}

/*! The log file grows to this size, then becomes server.log.1 and starts anew. */
const qint64 kLogLimit = 4 * 1024 * 1024;

/*!
 * Where the log goes: the console, unless the server is the service and has
 * none, and the log file when there is one.
 */
struct LogSink
{
    QMutex mutex;
    bool console = true;
    QFile file;
};

LogSink &sink()
{
    static LogSink instance;
    return instance;
}

void rotateLog(LogSink &log)
{
    const QString path = log.file.fileName();
    log.file.close();
    QFile::remove(path + QStringLiteral(".1"));
    // The admin panel may be reading it right now; then the file goes on
    // growing a little longer and the next line tries again.
    QFile::rename(path, path + QStringLiteral(".1"));
    log.file.setFileName(path);
    log.file.open(QIODevice::WriteOnly | QIODevice::Append);
}

/*!
 * Writes log lines as UTF-8: the default handler of Qt uses the ANSI code page
 * on Windows, which turns Russian text into garbage in a console. Information
 * goes to stdout, where systemd picks it up as well.
 */
void logMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    // A warning from inside the file code below must not come back here.
    static thread_local bool inside = false;
    if (inside)
        return;
    inside = true;

    const QByteArray line = qFormatLogMessage(type, context, message).toUtf8() + '\n';
    LogSink &log = sink();
    QMutexLocker lock(&log.mutex);
    if (log.console) {
        FILE *stream = type == QtInfoMsg || type == QtDebugMsg ? stdout : stderr;
        std::fwrite(line.constData(), 1, size_t(line.size()), stream);
        std::fflush(stream);
    }
    if (log.file.isOpen()) {
        if (log.file.size() + line.size() > kLogLimit)
            rotateLog(log);
        log.file.write(line);
        log.file.flush();
    }
    inside = false;
}

void openLog(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    LogSink &log = sink();
    QMutexLocker lock(&log.mutex);
    log.file.setFileName(path);
    log.file.open(QIODevice::WriteOnly | QIODevice::Append);
}

QString readPassword()
{
    QTextStream in(stdin);
    return in.readLine().trimmed();
}

QString native(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

/*!
 * --import-db: the installer moves the base of the server that used to run in
 * a console, from the data folder of the user, to the service.
 */
int importDatabase(const QString &source, const ServerConfig &config)
{
    QString message;
    const Storage::ImportResult result =
        Storage::importDatabase(source, config.databasePath(), &message);
    if (result == Storage::ImportResult::Failed) {
        qCritical().noquote() << message;
        return 1;
    }
    qInfo().noquote() << message;
    return 0;
}

/*!
 * The whole server. On Windows the service runs it on the thread the service
 * control manager hands out (Service::run()), otherwise it is main() itself.
 */
int runServer(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("CalendarPlanner"));
    QCoreApplication::setApplicationName(QStringLiteral("CalendarServer"));
    QCoreApplication::setApplicationVersion(QStringLiteral(SERVER_VERSION));

    qSetMessagePattern(QStringLiteral("%{time yyyy-MM-dd hh:mm:ss} "
                                      "%{if-warning}[!] %{endif}%{if-critical}[!!] %{endif}"
                                      "%{message}"));
    const bool asService = Service::active();
    sink().console = !asService;
    qInstallMessageHandler(logMessage);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Сервер синхронизации календаря-планера: хранит аккаунты "
                       "и записи пользователей."));
    const QCommandLineOption helpOption = parser.addHelpOption();
    const QCommandLineOption versionOption = parser.addVersionOption();
    const ServerOptions options;
    parser.addOptions(options.all());

    // process() would print help and errors in the ANSI code page on Windows,
    // which garbles Russian text; they go through the UTF-8 streams instead.
    if (!parser.parse(QCoreApplication::arguments())) {
        err() << parser.errorText() << Qt::endl;
        return 2;
    }
    if (parser.isSet(helpOption)) {
        out() << parser.helpText() << Qt::flush;
        return 0;
    }
    if (parser.isSet(versionOption)) {
        out() << "CalendarServer " << SERVER_VERSION << Qt::endl;
        return 0;
    }

    ServerConfig config;
    QString problem;
    if (!config.apply(parser, options, &problem)) {
        err() << problem << Qt::endl;
        return 2;
    }
    // A service writes server.log, on Linux as well (where systemd starts it
    // with --service too): that is the log the admin panel shows.
    const QString logPath = config.resolvedLogFile(parser.isSet(options.service));
    if (!logPath.isEmpty())
        openLog(logPath);

    if (parser.isSet(options.importDb))
        return importDatabase(parser.value(options.importDb), config);

    Storage storage;
    if (!storage.open(config.databasePath(), &problem)) {
        qCritical().noquote() << QStringLiteral("Не удалось открыть базу %1: %2")
                                     .arg(native(config.databasePath()), problem);
        return 1;
    }

    // ---- administration commands ----
    if (parser.isSet(options.listUsers)) {
        const QList<UserInfo> users = storage.users();
        out() << "Аккаунтов: " << users.size() << Qt::endl;
        for (const UserInfo &user : users) {
            out() << "  " << user.login << "  «" << user.displayName << "»  записей: "
                  << storage.recordCount(user.id) << "  сеансов: " << storage.tokenCount(user.id)
                  << "  создан: " << user.createdAt.toLocalTime().toString(Qt::ISODate) << Qt::endl;
        }
        return 0;
    }
    if (parser.isSet(options.deleteUser)) {
        const UserInfo user = storage.userByLogin(parser.value(options.deleteUser));
        if (!user.isValid()) {
            err() << "Нет такого аккаунта" << Qt::endl;
            return 1;
        }
        if (!storage.deleteUser(user.id)) {
            err() << "Не удалось удалить: " << storage.lastError() << Qt::endl;
            return 1;
        }
        out() << "Аккаунт " << user.login << " удалён" << Qt::endl;
        return 0;
    }
    if (parser.isSet(options.setPassword)) {
        const UserInfo user = storage.userByLogin(parser.value(options.setPassword));
        if (!user.isValid()) {
            err() << "Нет такого аккаунта" << Qt::endl;
            return 1;
        }
        out() << "Новый пароль: " << Qt::flush;
        const QString password = readPassword();
        const QString passwordProblem = Api::passwordProblem(password);
        if (!passwordProblem.isEmpty()) {
            err() << passwordProblem << Qt::endl;
            return 1;
        }
        storage.setPasswordHash(user.id, Api::hashPassword(password, config.passwordIterations));
        storage.removeOtherTokens(user.id, QByteArray());
        out() << "Пароль изменён, все устройства аккаунта должны войти заново" << Qt::endl;
        return 0;
    }

    // ---- the server itself ----
    // One server per base: a second one on the same folder would write into
    // the same file, and the admin panel would not know which of them to ask.
    RuntimeFile runtime(config.resolvedDataDir());
    if (!runtime.claim(&problem)) {
        qCritical().noquote() << QStringLiteral("Сервер не запущен: %1").arg(problem);
        return 1;
    }

    Api api(&storage, config);
    HttpServer server;
    server.setMaxBodySize(qint64(config.maxBodyMb) * 1024 * 1024);
    server.setHandler([&api](const HttpRequest &request) { return api.handle(request); });

    if (!server.listen(config.host, config.port)) {
        qCritical().noquote() << QStringLiteral("Не удалось занять порт %1: %2")
                                     .arg(config.port)
                                     .arg(server.errorString());
        return 1;
    }

    std::signal(SIGINT, requestQuit);
    std::signal(SIGTERM, requestQuit);

    // What the admin panel cannot learn from the settings: the port given on
    // the command line, the log file, the process.
    RuntimeInfo info;
    info.port = server.serverPort();
    info.host = config.host.toString();
    info.logFile = logPath;
    info.version = QStringLiteral(SERVER_VERSION);
    info.service = asService;
    runtime.publish(info);

    // The service control manager asks to stop on a thread of its own; the
    // flag it raises is picked up here, on the thread of the event loop.
    QTimer stopWatch;
    if (asService) {
        QObject::connect(&stopWatch, &QTimer::timeout, &app, [] {
            if (Service::stopRequested())
                QCoreApplication::quit();
        });
        stopWatch.start(200);
        Service::reportRunning();
    }

    qInfo().noquote() << QStringLiteral("CalendarServer %1 слушает порт %2%3")
                             .arg(QStringLiteral(SERVER_VERSION))
                             .arg(server.serverPort())
                             .arg(asService ? QStringLiteral(" (служба)") : QString());
    qInfo().noquote() << "База данных:" << native(config.databasePath());
    qInfo().noquote() << "Регистрация:" << (config.allowRegistration ? "открыта" : "закрыта");
    const QStringList addresses = config.clientAddresses(server.serverPort());
    if (!addresses.isEmpty()) {
        qInfo().noquote() << "Адрес для клиентов:";
        for (const QString &address : addresses)
            qInfo().noquote() << "   " << address;
    }

    const int code = app.exec();
    server.close();
    storage.close();
    runtime.release();
    qInfo().noquote() << "Сервер остановлен";
    return code;
}

} // namespace

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    // --service is looked for before Qt is up: the whole server then runs on
    // the thread the service control manager hands out, not on this one.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--service") == 0)
            return Service::run(argc, argv, runServer);
    }
    SetConsoleOutputCP(CP_UTF8);
#endif
    return runServer(argc, argv);
}
