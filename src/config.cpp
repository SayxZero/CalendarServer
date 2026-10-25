#include "config.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

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

    allowRegistration = ini.value(QStringLiteral("allow_registration"), allowRegistration).toBool();
    maxBodyMb = qBound(1, ini.value(QStringLiteral("max_body_mb"), maxBodyMb).toInt(), 1024);
    tokenLifetimeDays = qMax(1, ini.value(QStringLiteral("token_lifetime_days"),
                                          tokenLifetimeDays).toInt());
    ini.endGroup();
    return true;
}

QString ServerConfig::resolvedDataDir() const
{
    if (!dataDir.isEmpty())
        return QDir(dataDir).absolutePath();
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString ServerConfig::databasePath() const
{
    return QDir(resolvedDataDir()).filePath(QStringLiteral("server.db"));
}
