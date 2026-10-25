#include "config.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QNetworkInterface>
#include <QSettings>
#include <QStandardPaths>

ServerOptions::ServerOptions()
    : config({ QStringLiteral("c"), QStringLiteral("config") },
             QStringLiteral("INI-файл с настройками (группа [server])."), QStringLiteral("file"))
    , host(QStringLiteral("host"), QStringLiteral("Адрес, на котором слушать (по умолчанию все)."),
           QStringLiteral("address"))
    , port({ QStringLiteral("p"), QStringLiteral("port") },
           QStringLiteral("TCP-порт (по умолчанию %1).").arg(ServerConfig::defaultPort()),
           QStringLiteral("port"))
    , dataDir({ QStringLiteral("d"), QStringLiteral("data-dir") },
              QStringLiteral("Папка для server.db."), QStringLiteral("path"))
    , logFile(QStringLiteral("log-file"),
              QStringLiteral("Писать журнал ещё и в этот файл (относительный путь - от папки "
                             "данных)."),
              QStringLiteral("file"))
    , noRegistration(QStringLiteral("no-registration"),
                     QStringLiteral("Запретить создание новых аккаунтов."))
    , service(QStringLiteral("service"),
              QStringLiteral("Работать службой: журнал пишется и в server.log папки данных. На "
                             "Windows - службой без консоли, так сервер запускает диспетчер "
                             "служб; из консоли с этим ключом сервер не запускается."))
    , listUsers(QStringLiteral("list-users"), QStringLiteral("Показать аккаунты и выйти."))
    , deleteUser(QStringLiteral("delete-user"),
                 QStringLiteral("Удалить аккаунт вместе с данными и выйти."), QStringLiteral("login"))
    , setPassword(QStringLiteral("set-password"),
                  QStringLiteral("Задать новый пароль аккаунта (читается из stdin) и выйти."),
                  QStringLiteral("login"))
    , importDb(QStringLiteral("import-db"),
               QStringLiteral("Перенести базу из файла, если в своей ещё нет аккаунтов, и выйти. "
                              "Так установщик переносит базу прежнего сервера."),
               QStringLiteral("file"))
{
}

QList<QCommandLineOption> ServerOptions::all() const
{
    return { config, host, port, dataDir, logFile, noRegistration, service,
             listUsers, deleteUser, setPassword, importDb };
}

bool ServerConfig::loadIni(const QString &path, QString *error)
{
    if (!QFileInfo::exists(path)) {
        if (error)
            *error = QStringLiteral("Файл настроек не найден: %1").arg(path);
        return false;
    }

    QSettings ini(path, QSettings::IniFormat);
    if (ini.status() != QSettings::NoError) {
        if (error)
            *error = QStringLiteral("Не удалось прочитать %1").arg(path);
        return false;
    }
    configFile = QFileInfo(path).absoluteFilePath();

    ini.beginGroup(QStringLiteral("server"));
    if (ini.contains(QStringLiteral("host"))) {
        const QString text = ini.value(QStringLiteral("host")).toString().trimmed();
        if (!text.isEmpty() && !host.setAddress(text)) {
            if (error)
                *error = QStringLiteral("Неверный адрес host: %1").arg(text);
            return false;
        }
    }
    port = quint16(ini.value(QStringLiteral("port"), port).toUInt());

    // A relative folder is taken from where the INI file lies, not from the
    // working directory of whoever started the service.
    const QString dir = ini.value(QStringLiteral("data_dir")).toString().trimmed();
    if (!dir.isEmpty()) {
        dataDir = QDir::isAbsolutePath(dir) ? dir
                                            : QFileInfo(path).absoluteDir().absoluteFilePath(dir);
    }
    const QString log = ini.value(QStringLiteral("log_file")).toString().trimmed();
    if (!log.isEmpty())
        logFile = log;

    allowRegistration = ini.value(QStringLiteral("allow_registration"), allowRegistration).toBool();
    maxBodyMb = qBound(1, ini.value(QStringLiteral("max_body_mb"), maxBodyMb).toInt(), 1024);
    tokenLifetimeDays = qMax(1, ini.value(QStringLiteral("token_lifetime_days"),
                                          tokenLifetimeDays).toInt());
    ini.endGroup();
    return true;
}

bool ServerConfig::apply(const QCommandLineParser &parser, const ServerOptions &options,
                         QString *error)
{
    if (parser.isSet(options.config) && !loadIni(parser.value(options.config), error))
        return false;
    if (parser.isSet(options.host) && !host.setAddress(parser.value(options.host))) {
        if (error)
            *error = QStringLiteral("Неверный адрес: %1").arg(parser.value(options.host));
        return false;
    }
    if (parser.isSet(options.port)) {
        bool ok = false;
        const uint value = parser.value(options.port).toUInt(&ok);
        if (!ok || value == 0 || value > 65535) {
            if (error)
                *error = QStringLiteral("Неверный порт: %1").arg(parser.value(options.port));
            return false;
        }
        port = quint16(value);
    }
    if (parser.isSet(options.dataDir))
        dataDir = parser.value(options.dataDir);
    if (parser.isSet(options.logFile))
        logFile = parser.value(options.logFile);
    if (parser.isSet(options.noRegistration))
        allowRegistration = false;
    return true;
}

QString ServerConfig::resolvedDataDir() const
{
    if (!dataDir.isEmpty())
        return QDir(dataDir).absolutePath();
    // The folder of the application named CalendarServer, whoever asks: the
    // admin panel is an application of a name of its own.
    const QDir own(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    return QDir::cleanPath(own.absoluteFilePath(QStringLiteral("../CalendarServer")));
}

QString ServerConfig::databasePath() const
{
    return QDir(resolvedDataDir()).filePath(QStringLiteral("server.db"));
}

QString ServerConfig::resolvedLogFile(bool service) const
{
    const QString file = !logFile.isEmpty() ? logFile
                         : service          ? QStringLiteral("server.log")
                                            : QString();
    if (file.isEmpty())
        return QString();
    return QDir::isAbsolutePath(file) ? QDir::cleanPath(file)
                                      : QDir(resolvedDataDir()).absoluteFilePath(file);
}

QStringList ServerConfig::clientAddresses(quint16 actualPort) const
{
    const quint16 shown = actualPort ? actualPort : port;
    QStringList result;
    if (host != QHostAddress::Any && host != QHostAddress::AnyIPv4
        && host != QHostAddress::AnyIPv6) {
        result << QStringLiteral("%1:%2").arg(host.toString()).arg(shown);
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
            result << QStringLiteral("%1:%2  (%3)").arg(address.toString()).arg(shown)
                          .arg(iface.humanReadableName());
        }
    }
    return result;
}
