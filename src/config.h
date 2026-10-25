#pragma once

#include <QCommandLineOption>
#include <QHostAddress>
#include <QList>
#include <QString>
#include <QStringList>

class QCommandLineParser;

/*!
 * The command line of the server, in one place: the server parses its own with
 * it, and the admin panel parses the one the service was installed with, to
 * find the same base the service works on.
 */
struct ServerOptions
{
    ServerOptions();

    /*! Every option, in the order the help lists them. */
    QList<QCommandLineOption> all() const;

    const QCommandLineOption config;
    const QCommandLineOption host;
    const QCommandLineOption port;
    const QCommandLineOption dataDir;
    const QCommandLineOption logFile;
    const QCommandLineOption noRegistration;
    const QCommandLineOption service;
    const QCommandLineOption listUsers;
    const QCommandLineOption deleteUser;
    const QCommandLineOption setPassword;
    const QCommandLineOption importDb;
};

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

    /*!
     * Where the log goes besides the console, relative to the data folder.
     * Empty: nowhere, except for the service, which writes server.log there.
     */
    QString logFile;

    /*! The INI file the settings came from, empty when there was none. */
    QString configFile;

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

    /*!
     * The settings a parsed command line gives: the INI file of --config, then
     * the options on top of it. Returns false with @p error on a wrong value.
     */
    bool apply(const QCommandLineParser &parser, const ServerOptions &options, QString *error);

    QString databasePath() const;
    QString resolvedDataDir() const;

    /*! The log file, absolute; with @p service the default server.log as well. */
    QString resolvedLogFile(bool service) const;

    /*!
     * The addresses a phone in the same network can use to reach the server,
     * with the names of the network interfaces: what goes into the planner.
     */
    QStringList clientAddresses(quint16 actualPort = 0) const;
};
