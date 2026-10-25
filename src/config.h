#pragma once

#include <QHostAddress>
#include <QString>

/*!
 * Everything the server can be told at start: an optional INI file first, the
 * command line on top of it.
 */
struct ServerConfig
{
    static constexpr quint16 defaultPort() { return 8765; }

    QHostAddress host = QHostAddress::Any;
    quint16 port = defaultPort();

    /*! Folder of server.db; the platform data folder when empty. */
    QString dataDir;

    /*! Anyone who reaches the server may create an account. */
    bool allowRegistration = true;

    /*! Largest request body, the whole planner of one user in one sync. */
    int maxBodyMb = 64;

    /*! A token that was not used for this long stops working. */
    int tokenLifetimeDays = 365;

    /*! PBKDF2 rounds; lowered only by the tests, which hash many passwords. */
    int passwordIterations = 120000;

    /*! Reads the [server] group of @p path; returns false with @p error when it fails. */
    bool loadIni(const QString &path, QString *error);

    QString databasePath() const;
    QString resolvedDataDir() const;
};
