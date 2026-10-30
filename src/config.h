#pragma once

#include <QCommandLineOption>
#include <QHostAddress>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

class QCommandLineParser;

/*! An address and its prefix length, as QHostAddress::isInSubnet() takes it; one address is a whole prefix. */
using Subnet = QPair<QHostAddress, int>;

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
    const QCommandLineOption trustedProxies;
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

    /*!
     * Reverse proxies in front of the server (the backend of the site, nginx),
     * by address or subnet. A request that comes from one of them is the
     * client's its X-Forwarded-For names - for the wrong passwords and in the
     * log; from anywhere else the header means nothing. Empty: every request
     * is the address its connection came from, as before.
     */
    QList<Subnet> trustedProxies;

    /*! Largest request body, the whole planner of one user in one sync. */
    int maxBodyMb = 64;

    /*! A token that was not used for this long stops working. */
    int tokenLifetimeDays = 365;

    /*! PBKDF2 rounds; lowered only by the tests, which hash many passwords. */
    int passwordIterations = 120000;

    // ---- load ----------------------------------------------------------------
    /*!
     * Threads that answer requests; 0 - as many as the processor runs at
     * once, and never fewer than two.
     */
    int threads = 0;

    /*!
     * How requests wait for a thread: "fair", "fifo", "round_robin",
     * "least_loaded" (workerpool.h). Anything but the default is for
     * comparisons.
     */
    QString dispatch = QStringLiteral("fair");

    /*! Connections at once; past it new ones wait in the queue of the system. */
    int maxConnections = 1024;

    /*!
     * Request bodies held in memory at once, MB; past it the next bodies wait.
     * 0 - an eighth of the memory the process may have (the RAM, or less
     * when a job object or a cgroup limits it), between 16 MB and 1 GB, and
     * never less than one body of the largest size.
     */
    int bufferMb = 0;

    /*! A request that waited for a thread longer than this gets 503 instead of an answer. */
    int maxQueueWaitSeconds = 30;

    /*!
     * Group commit: the writes of up to this many requests go to the disk in
     * one transaction, with one fsync. 0 - every request commits on its own.
     * On a hard disk it is what the number of writes a second depends on.
     */
    int groupCommit = 64;

    /*!
     * "full" - a commit is on the disk before the answer leaves (an fsync
     * each); "normal" - the log is synced at checkpoints, and a power cut may
     * lose the last seconds of exchanges, never the base.
     */
    QString syncMode = QStringLiteral("full");

    /*! What a request body weighs in memory while it is answered, in its own sizes. */
    static constexpr int bodyMemoryFactor() { return 8; }

    /*! The threads this config asks for, 0 resolved. */
    int resolvedThreads() const;

    /*! The budget of request bodies in bytes, 0 resolved; a body counts bodyMemoryFactor() times. */
    qint64 resolvedBufferBytes() const;

    /*!
     * The load settings as they go into the [server] group of an INI file
     * (the admin panel writes them, the benchmark recommends them), and
     * which keys those are.
     */
    static QStringList loadKeys();
    QString loadValue(const QString &key) const;

    /*!
     * The memory this process may have: the RAM of the machine, or the limit
     * of the job object (Windows) or the cgroup (Linux) it runs in when that
     * is smaller. -1 when unknown.
     */
    static qint64 memoryLimit();

    /*!
     * "127.0.0.1, ::1, 10.0.0.0/8" - addresses and subnets separated by commas
     * or spaces - into @p proxies. Returns false with @p error naming the
     * entry that is neither; a bare address is that address alone, never the
     * network Qt would guess from "10" or "192.168".
     */
    static bool parseTrustedProxies(const QString &text, QList<Subnet> *proxies, QString *error);

    /*! trustedProxies as they would be written, for the log. */
    QString trustedProxiesText() const;

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
