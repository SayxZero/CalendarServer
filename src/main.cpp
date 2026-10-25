#include "api.h"
#include "config.h"
#include "httpserver.h"
#include "storage.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QNetworkInterface>
#include <QTextStream>

#include <csignal>
#include <cstdio>

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

/*! The addresses a phone in the same network can use to reach this machine. */
QStringList reachableAddresses(const QHostAddress &bound, quint16 port)
{
    QStringList result;
    if (bound != QHostAddress::Any && bound != QHostAddress::AnyIPv4
        && bound != QHostAddress::AnyIPv6) {
        result << QStringLiteral("%1:%2").arg(bound.toString()).arg(port);
        return result;
    }
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            result << QStringLiteral("%1:%2  (%3)").arg(address.toString()).arg(port)
                          .arg(iface.humanReadableName());
        }
    }
    return result;
}

/*!
 * Writes log lines as UTF-8: the default handler of Qt uses the ANSI code page
 * on Windows, which turns Russian text into garbage in a console. Information
 * goes to stdout, where systemd picks it up as well.
 */
void logToConsole(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    const QByteArray line = qFormatLogMessage(type, context, message).toUtf8() + '\n';
    FILE *stream = type == QtInfoMsg || type == QtDebugMsg ? stdout : stderr;
    std::fwrite(line.constData(), 1, size_t(line.size()), stream);
    std::fflush(stream);
}

QString readPassword()
{
    QTextStream in(stdin);
    return in.readLine().trimmed();
}

} // namespace

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    SetConsoleOutputCP(CP_UTF8);
#endif
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("CalendarPlanner"));
    QCoreApplication::setApplicationName(QStringLiteral("CalendarServer"));
    QCoreApplication::setApplicationVersion(QStringLiteral(SERVER_VERSION));

    qSetMessagePattern(QStringLiteral("%{time yyyy-MM-dd hh:mm:ss} "
                                      "%{if-warning}[!] %{endif}%{if-critical}[!!] %{endif}"
                                      "%{message}"));
    qInstallMessageHandler(logToConsole);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Сервер синхронизации календаря-планера: хранит аккаунты "
                       "и записи пользователей."));
    const QCommandLineOption helpOption = parser.addHelpOption();
    const QCommandLineOption versionOption = parser.addVersionOption();

    const QCommandLineOption configOption(
        { QStringLiteral("c"), QStringLiteral("config") },
        QStringLiteral("INI-файл с настройками (группа [server])."), QStringLiteral("file"));
    const QCommandLineOption hostOption(
        QStringLiteral("host"),
        QStringLiteral("Адрес, на котором слушать (по умолчанию все)."), QStringLiteral("address"));
    const QCommandLineOption portOption(
        { QStringLiteral("p"), QStringLiteral("port") },
        QStringLiteral("TCP-порт (по умолчанию %1).").arg(ServerConfig::defaultPort()),
        QStringLiteral("port"));
    const QCommandLineOption dataOption(
        { QStringLiteral("d"), QStringLiteral("data-dir") },
        QStringLiteral("Папка для server.db."), QStringLiteral("path"));
    const QCommandLineOption noRegistrationOption(
        QStringLiteral("no-registration"),
        QStringLiteral("Запретить создание новых аккаунтов."));
    const QCommandLineOption listUsersOption(
        QStringLiteral("list-users"), QStringLiteral("Показать аккаунты и выйти."));
    const QCommandLineOption deleteUserOption(
        QStringLiteral("delete-user"),
        QStringLiteral("Удалить аккаунт вместе с данными и выйти."), QStringLiteral("login"));
    const QCommandLineOption setPasswordOption(
        QStringLiteral("set-password"),
        QStringLiteral("Задать новый пароль аккаунта (читается из stdin) и выйти."),
        QStringLiteral("login"));

    parser.addOptions({ configOption, hostOption, portOption, dataOption, noRegistrationOption,
                        listUsersOption, deleteUserOption, setPasswordOption });
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
    if (parser.isSet(configOption) && !config.loadIni(parser.value(configOption), &problem)) {
        err() << problem << Qt::endl;
        return 2;
    }
    if (parser.isSet(hostOption) && !config.host.setAddress(parser.value(hostOption))) {
        err() << "Неверный адрес: " << parser.value(hostOption) << Qt::endl;
        return 2;
    }
    if (parser.isSet(portOption)) {
        bool ok = false;
        const uint port = parser.value(portOption).toUInt(&ok);
        if (!ok || port == 0 || port > 65535) {
            err() << "Неверный порт: " << parser.value(portOption) << Qt::endl;
            return 2;
        }
        config.port = quint16(port);
    }
    if (parser.isSet(dataOption))
        config.dataDir = parser.value(dataOption);
    if (parser.isSet(noRegistrationOption))
        config.allowRegistration = false;

    Storage storage;
    if (!storage.open(config.databasePath(), &problem)) {
        err() << "Не удалось открыть базу " << config.databasePath() << ": " << problem << Qt::endl;
        return 1;
    }

    // ---- administration commands ----
    if (parser.isSet(listUsersOption)) {
        const QList<UserInfo> users = storage.users();
        out() << "Аккаунтов: " << users.size() << Qt::endl;
        for (const UserInfo &user : users) {
            out() << "  " << user.login << "  «" << user.displayName << "»  записей: "
                  << storage.recordCount(user.id) << "  сеансов: " << storage.tokenCount(user.id)
                  << "  создан: " << user.createdAt.toLocalTime().toString(Qt::ISODate) << Qt::endl;
        }
        return 0;
    }
    if (parser.isSet(deleteUserOption)) {
        const UserInfo user = storage.userByLogin(parser.value(deleteUserOption));
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
    if (parser.isSet(setPasswordOption)) {
        const UserInfo user = storage.userByLogin(parser.value(setPasswordOption));
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

    qInfo().noquote() << QStringLiteral("CalendarServer %1 слушает порт %2")
                             .arg(QStringLiteral(SERVER_VERSION))
                             .arg(server.serverPort());
    qInfo().noquote() << "База данных:" << config.databasePath();
    qInfo().noquote() << "Регистрация:" << (config.allowRegistration ? "открыта" : "закрыта");
    const QStringList addresses = reachableAddresses(config.host, server.serverPort());
    if (!addresses.isEmpty()) {
        qInfo().noquote() << "Адрес для клиентов:";
        for (const QString &address : addresses)
            qInfo().noquote() << "   " << address;
    }

    const int code = app.exec();
    server.close();
    storage.close();
    qInfo().noquote() << "Сервер остановлен";
    return code;
}
