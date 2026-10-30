#pragma once

#include "shares.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QSet>
#include <QSqlDatabase>
#include <QString>

#include <functional>
#include <memory>

struct UserInfo
{
    qint64 id = 0;
    QString login;
    QString displayName;
    QDateTime createdAt;
    qint64 revision = 0;   //!< the newest change stored for this user

    bool isValid() const { return id > 0; }
};

/*!
 * One record of a user's planner as the server sees it: an opaque JSON object
 * with a kind ("event", "task", ...) and an id. The server never looks inside;
 * it only decides which version is the newest.
 */
struct SyncRecord
{
    QString kind;
    QString id;
    bool deleted = false;
    qint64 modified = 0;   //!< ms since the epoch, set by the device that made the change
    qint64 revision = 0;   //!< server side counter, filled in on the way out
    QByteArray data;       //!< compact JSON object, empty for a deletion
    QString device;

    /*!
     * The device says the user has seen that the owner of a shared calendar
     * deleted this record and asked for it back. Without it a change of a
     * deleted record is refused, and the question would be asked again forever.
     */
    bool restore = false;

    QString key() const { return kind + QLatin1Char('/') + id; }
};

/*! What one exchange carried, for the activity statistics (Storage::noteSync()). */
struct SyncTraffic
{
    int changesIn = 0;    //!< records the device sent
    int changesOut = 0;   //!< records it got back
    qint64 bytesIn = 0;   //!< size of the request body
    qint64 bytesOut = 0;  //!< size of the answer
};

/*! What a database file holds, read without changing it (Storage::inspect()). */
struct DatabaseInfo
{
    bool ok = false;        //!< a base of this server that opened and passed the checks
    QString error;          //!< why not, for people
    int schemaVersion = 0;
    int users = 0;
    int records = 0;        //!< not counting tombstones
    QDateTime lastActivity; //!< the newest sync or use of a session it remembers
};

/*! One session of an account (a row of tokens). */
struct SessionInfo
{
    QByteArray tokenHash;   //!< stays in the server: Api shows an id made from it
    QString device;
    QDateTime createdAt;
    QDateTime lastUsedAt;
};

struct SyncOutcome
{
    qint64 revision = 0;       //!< the cursor the device keeps for its next sync
    QList<SyncRecord> changes; //!< what the device does not have yet
    int accepted = 0;
    int rejected = 0;

    /*!
     * Changes that could not be applied to a shared calendar and why, so that
     * the planner can ask the user what to do with them.
     */
    QList<ShareConflict> conflicts;
    bool ok = false;
};

/*!
 * How the writes of one base went: how long they waited for their turn and
 * for the commit, and how many operations a commit carried.
 */
struct GateStats
{
    qint64 writes = 0;      //!< write operations (transact()) that went through
    qint64 waitNs = 0;      //!< all of them waiting for their turn and commit together
    qint64 holdNs = 0;      //!< transactions holding the write lock
    qint64 maxWaitNs = 0;   //!< the longest wait since the previous takeGateStats()
    qint64 commits = 0;     //!< transactions committed: writes / commits = operations per fsync
};

struct BaseShared;

/*!
 * SQLite persistence of the server: accounts, login tokens and the records of
 * every user.
 *
 * One Storage is one connection, and it belongs to the thread that opened it:
 * the server gives every worker thread a Storage of its own (Qt allows a
 * connection on one thread only). Every Storage of one process on the same
 * file shares a BaseShared - the gate writes go through one at a time
 * (Storage::Write) and what is counted in memory to be written later.
 */
class Storage
{
public:
    Storage();
    ~Storage();

    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;

    /*!
     * Create: brings the schema up to this version, as the first connection
     * of a process does. Attach: one more connection to a base another
     * Storage of the process has already prepared - a worker thread's.
     */
    enum class OpenMode { Create, Attach };

    bool open(const QString &path, QString *error = nullptr, OpenMode mode = OpenMode::Create);
    void close();
    QString lastError() const { return m_lastError; }

    /*!
     * What a commit waits for (PRAGMA synchronous). Full: the commit is on
     * the disk when it returns - an fsync each time. Normal: the WAL is synced
     * at checkpoints only; a power cut loses the last commits (never the base),
     * and on a hard disk a commit is a hundred times cheaper. Set on the
     * first connection, it holds for every connection of the process to the base.
     */
    enum class Durability { Full, Normal };
    void setDurability(Durability durability);

    /*! PRAGMA synchronous of this connection as SQLite has it: 2 full, 1 normal. */
    int synchronousMode();

    /*!
     * Runs @p work - the writes of one operation, all or none - in a write
     * transaction, and returns once it is committed. @p work gets the Storage
     * to work on, which need not be this one, and must use only it.
     *
     * With group commit on (enableGroupCommit()) the work runs on the commit
     * thread of the base, in one transaction with the operations other
     * threads sent meanwhile, each under a savepoint of its own: one fsync
     * for all of them, and a failure rolls back only its own operation.
     * Without it the work runs here, in a Write of this connection. Inside
     * a transaction of this connection already, it is a part of it.
     */
    bool transact(const std::function<bool(Storage &)> &work);

    /*!
     * Group commit for the base of this Storage: a thread of its own with a
     * connection of its own runs the operations of transact() in batches of
     * up to @p batch per transaction. On a disk with slow fsync (a hard disk)
     * that multiplies how many writes a second the server takes.
     */
    bool enableGroupCommit(int batch, QString *error = nullptr);
    void disableGroupCommit();

    /*!
     * One write transaction (BEGIN IMMEDIATE). The writes of every Storage of
     * the process on this base pass one gate one at a time: SQLite lets only
     * one writer in anyway, and a mutex hands the turn on in order and at
     * once, where SQLITE_BUSY makes the others sleep and try again. Another
     * process (the admin panel) is waited for by busy_timeout. A Write inside
     * a Write of the same Storage joins the outer one; the outermost commits,
     * and a failure anywhere rolls the whole of it back.
     */
    class Write
    {
        friend class Storage;

    public:
        explicit Write(Storage &storage);
        ~Write();

        Write(const Write &) = delete;
        Write &operator=(const Write &) = delete;

        bool ok() const { return m_ok; }

        /*! Commits the outermost transaction; an inner one only reports how it stands. */
        bool commit();

        /*! Something failed: the outermost Write rolls back instead of committing. */
        void fail();

    private:
        Storage &m_storage;
        bool m_outer = false;
        bool m_ok = false;
        bool m_done = false;
        bool m_counted = true;   //!< one operation; a batch of the commit thread counts its own
        qint64 m_heldSince = 0;
    };

    /*!
     * Writes what was counted in memory: the activity of exchanges
     * (noteSync()) and the last use of sessions. The server calls it every
     * few seconds and on the way out; it is cheap when there is nothing.
     */
    bool flushPending();

    /*! The gate of this base since the start; the longest wait is reset by the call. */
    GateStats takeGateStats();

    /*! The version this build writes into PRAGMA user_version. */
    static int schemaVersion();

    // ---- whole files ---------------------------------------------------------
    /*!
     * Opens @p path read-only and tells what it is. With @p check the whole file
     * is read through (PRAGMA quick_check) - for a backup about to be restored.
     */
    static DatabaseInfo inspect(const QString &path, bool check = false);

    /*!
     * Writes a consistent copy of the base @p from into the new file @p to
     * (VACUUM INTO): it may be taken while the server works on @p from. The copy
     * is a single file in the rollback journal mode, with no -wal beside it.
     */
    static bool copyDatabase(const QString &from, const QString &to, QString *error);

    /*! Deletes a base together with its -wal, -shm and -journal files. */
    static bool removeDatabase(const QString &path, QString *error);

    enum class ImportResult { Imported, Skipped, Failed };

    /*!
     * --import-db: brings the base @p source to @p target unless @p target has
     * accounts already - how the installer moves the base of a server that ran
     * in a console to the service. @p source is only read. @p message says what
     * happened, for the log.
     */
    static ImportResult importDatabase(const QString &source, const QString &target,
                                       QString *message);

    // ---- users -------------------------------------------------------------
    UserInfo userByLogin(const QString &login);
    UserInfo userById(qint64 id);
    QList<UserInfo> users();

    /*! Returns an invalid user when the login is taken. */
    UserInfo createUser(const QString &login, const QString &displayName,
                        const QString &passwordHash);
    QString passwordHash(qint64 userId);
    bool setPasswordHash(qint64 userId, const QString &hash);
    bool setDisplayName(qint64 userId, const QString &name);

    /*! Deletes the account together with its tokens and records. */
    bool deleteUser(qint64 userId);

    // ---- tokens ------------------------------------------------------------
    bool addToken(const QByteArray &tokenHash, qint64 userId, const QString &device);

    /*!
     * The user a token belongs to, or 0. A token unused for longer than
     * @p lifetimeDays is dropped here; a live one gets its last use stamped.
     */
    qint64 userForToken(const QByteArray &tokenHash, int lifetimeDays);
    bool removeToken(const QByteArray &tokenHash);

    /*! Logs out every other device of the user, used after a password change. */
    bool removeOtherTokens(qint64 userId, const QByteArray &keepHash);
    int tokenCount(qint64 userId);

    /*! The sessions of the user still alive, the most recently used first. */
    QList<SessionInfo> sessions(qint64 userId, int lifetimeDays);

    // ---- records -----------------------------------------------------------
    /*!
     * Stores @p incoming for the user and returns what changed since @p since.
     *
     * The newest version of a record wins, judged by the moment the device made
     * the change. A change older than the stored one is rejected and the stored
     * version goes back to the device instead. Changes accepted in this call are
     * not sent back: the device has them already.
     */
    SyncOutcome sync(qint64 userId, qint64 since, const QList<SyncRecord> &incoming,
                     const QString &device);

    /*!
     * Counts one exchange of the user into the activity of today: how many
     * accounts sync a day, a week and a month is what the admin panel shows.
     * It is counted in memory and written by flushPending(): a write of its
     * own would put every exchange, a plain download too, in the line at the
     * gate.
     */
    void noteSync(qint64 userId, const SyncTraffic &traffic);

    int recordCount(qint64 userId, bool includeDeleted = false);

    /*! The stored JSON of one record, empty when there is none or it is deleted. */
    QByteArray recordData(qint64 userId, const QString &kind, const QString &id);

    /*!
     * The whole stored record of @p userId - deleted or not, with its stamps -
     * for GET /api/record. @p found says whether there is one at all.
     */
    SyncRecord record(qint64 userId, const QString &kind, const QString &id, bool *found)
    {
        return recordAt(userId, kind, id, found);
    }

    /*! Who shares which calendar with whom; see shares.h. */
    ShareStore &shares() { return *m_shares; }

private:
    class Committer;
    friend struct BaseShared;

    bool exec(const QString &statement);
    SyncRecord recordAt(qint64 userId, const QString &kind, const QString &id, bool *found);

    /*!
     * The writing half of sync(), inside a transaction: applies @p incoming
     * and leaves the new cursor in @p revision, what was taken in @p accepted
     * and the stored versions that won in @p rejected.
     */
    bool applyChanges(qint64 userId, const QList<SyncRecord> &incoming, const QString &device,
                      qint64 *revision, QSet<QString> *accepted, QList<SyncRecord> *rejected,
                      QList<ShareConflict> *conflicts);

    /*! The changes of @p userId in (since, upTo], for the answer of sync(). */
    bool readChanges(qint64 userId, qint64 since, qint64 upTo, const QSet<QString> &skip,
                     QList<SyncRecord> *changes, QSet<QString> *listed);

    QSqlDatabase m_db;
    QString m_connectionName;
    QString m_lastError;
    std::unique_ptr<ShareStore> m_shares;
    std::shared_ptr<BaseShared> m_base;
    int m_writeDepth = 0;
    bool m_writeFailed = false;
    bool m_ownsCommitter = false;   //!< enableGroupCommit() was called here; close() ends it
};
