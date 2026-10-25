#pragma once

#include "shares.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QSqlDatabase>
#include <QString>

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
 * SQLite persistence of the server: accounts, login tokens and the records of
 * every user. All calls happen on the one thread the server runs on.
 */
class Storage
{
public:
    Storage();
    ~Storage();

    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;

    bool open(const QString &path, QString *error = nullptr);
    void close();
    QString lastError() const { return m_lastError; }

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
     */
    bool noteSync(qint64 userId, const SyncTraffic &traffic);

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
    bool exec(const QString &statement);
    SyncRecord recordAt(qint64 userId, const QString &kind, const QString &id, bool *found);

    QSqlDatabase m_db;
    QString m_connectionName;
    QString m_lastError;
    std::unique_ptr<ShareStore> m_shares;
};
