#pragma once

#include <QDateTime>
#include <QLockFile>
#include <QString>

#include <memory>

/*!
 * What a server that runs leaves in its data folder, beside server.db.
 *
 *  - server.lock - held for as long as the server runs: a second server on the
 *    same base refuses to start, and the admin panel knows the base is in use
 *    before it replaces it with a backup. Neither the state of the service nor
 *    an answer on the configured port tells that: a server in a console may
 *    listen on a port of its own (-p) and still write into this very base.
 *  - server.run - JSON with the process, the port and the log file, the things
 *    the command line of a console server says and its INI file does not; the
 *    admin panel opened with --data-dir reads them from here.
 */
struct RuntimeInfo
{
    bool running = false;   //!< a live process holds server.lock
    qint64 pid = 0;
    quint16 port = 0;
    QString host;
    QString logFile;        //!< absolute; empty when the server keeps no log file
    QString version;
    QDateTime startedAt;
    bool service = false;
};

class RuntimeFile
{
public:
    explicit RuntimeFile(const QString &dataDir);
    ~RuntimeFile();

    static QString lockPath(const QString &dataDir);
    static QString infoPath(const QString &dataDir);

    /*!
     * Takes the folder for this process. Fails, with the process that holds
     * it in @p error, when another one is running on the same base.
     */
    bool claim(QString *error = nullptr);

    /*! Writes server.run; the folder has to be claimed first. */
    void publish(const RuntimeInfo &info);

    /*! Lets the folder go and removes server.run. */
    void release();

    /*!
     * What the folder says: whether a server holds it right now, and what it
     * wrote about itself. Read without taking the lock.
     */
    static RuntimeInfo read(const QString &dataDir);

private:
    QString m_dataDir;
    std::unique_ptr<QLockFile> m_lock;
};
