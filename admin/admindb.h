#pragma once

#include "shares.h"

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QSqlDatabase>
#include <QString>

/*! One account as the list of the admin panel shows it. */
struct UserSummary
{
    qint64 id = 0;
    QString login;
    QString displayName;
    QDateTime createdAt;
    qint64 revision = 0;
    int devices = 0;        //!< sessions, one per device that signed in
    int records = 0;        //!< not counting tombstones
    int calendars = 0;
    int events = 0;
    int tasks = 0;
    qint64 bytes = 0;       //!< what its records take, without the tombstones
    QDateTime lastSync;     //!< the newest exchange counted
    QDateTime lastSeen;     //!< the newest use of any of its sessions
    int syncsMonth = 0;     //!< exchanges in the last 30 days
};

/*! A device signed into an account: its session. */
struct DeviceSession
{
    QString name;
    QDateTime createdAt;
    QDateTime lastUsedAt;
};

/*! One record as it is stored, with its JSON already parsed. */
struct StoredRecord
{
    QString kind;
    QString id;
    bool deleted = false;
    qint64 modified = 0;
    qint64 revision = 0;
    QString device;
    QJsonObject data;
    qint64 size = 0;
};

/*!
 * A calendar of an account and where it comes from: its own, its own opened to
 * others, or the twin of somebody else's calendar.
 */
struct CalendarSummary
{
    enum class Role {
        Own,    //!< a calendar of this account that nobody else sees
        Open,   //!< its own, opened to other accounts (or once was)
        Twin,   //!< this account's copy of a calendar somebody opened to it
    };

    QString id;
    QString name;
    QString color;
    Role role = Role::Own;
    bool missing = false;    //!< known from the share only, the record has not come yet

    // Role::Open
    ShareMode mode = ShareMode::Custom;
    bool enabled = false;
    QList<ShareMemberInfo> members;

    // Role::Twin
    QString ownerLogin;
    QString ownerDisplayName;
    QString originName;      //!< how the owner calls the calendar
    ShareAccess access = ShareAccess::Read;

    int events = 0;
    int tasks = 0;
    int other = 0;           //!< completions and the rest that live in a calendar
};

/*! The counters of the statistics page, at one moment. */
struct ServerTotals
{
    bool tracked = false;      //!< the base counts exchanges (schema 3 and later)
    QDate trackedSince;        //!< the first day it has a count for

    int accounts = 0;
    int newAccounts = 0;       //!< created in the last 30 days

    int activeDay = 0;         //!< accounts that synced in the last 24 hours
    int activeWeek = 0;        //!< ... in the last 7 days
    int activeMonth = 0;       //!< ... in the last 30 days

    int syncsDay = 0;
    int syncsWeek = 0;
    int syncsMonth = 0;
    qint64 bytesInMonth = 0;
    qint64 bytesOutMonth = 0;
    qint64 changesInMonth = 0;

    int devices = 0;
    int devicesMonth = 0;      //!< sessions used in the last 30 days

    int records = 0;
    int tombstones = 0;
    QList<QPair<QString, int>> recordsByKind;

    int sharedCalendars = 0;   //!< opened and not closed again
    int shareMembers = 0;
    int liveLinks = 0;         //!< links that still let somebody in
};

/*! One day of the history: the points of the charts. */
struct DayActivity
{
    QDate day;
    int daily = 0;     //!< accounts that synced that day
    int weekly = 0;    //!< ... in the 7 days up to it
    int monthly = 0;   //!< ... in the 30 days up to it
    int syncs = 0;
    qint64 bytesIn = 0;
    qint64 bytesOut = 0;
    int newAccounts = 0;
    int accounts = 0;  //!< accounts that existed at the end of the day
};

/*! Where the space of the base goes. */
struct StorageUse
{
    qint64 file = 0;       //!< server.db
    qint64 wal = 0;        //!< server.db-wal, not yet written into the file
    qint64 shm = 0;
    qint64 pageSize = 0;
    qint64 pages = 0;
    qint64 freePages = 0;  //!< what a VACUUM would give back
    qint64 payload = 0;    //!< the JSON of all live records
    qint64 tombstones = 0; //!< count of deleted records kept for the devices
    struct Kind { QString kind; int count = 0; qint64 bytes = 0; };
    QList<Kind> byKind;
    QList<QPair<QString, qint64>> topUsers;   //!< login and bytes, biggest first
};

/*!
 * The base of the server as the admin panel reads it: on a connection of its
 * own, next to the server working on the same file. SQLite in WAL mode lets
 * them both; the only write here, a new password, takes the lock for a moment
 * and the server waits for it.
 *
 * The panel never creates the base and never changes its schema: that is the
 * server's business. A base of an older server (no activity table) is read
 * all the same, without the statistics of exchanges.
 */
class AdminDb
{
public:
    AdminDb();
    ~AdminDb();

    AdminDb(const AdminDb &) = delete;
    AdminDb &operator=(const AdminDb &) = delete;

    bool open(const QString &path, QString *error);
    void close();
    bool isOpen() const { return m_db.isOpen(); }
    QString path() const { return m_path; }
    QString lastError() const { return m_lastError; }
    int schemaVersion() const { return m_schemaVersion; }
    bool tracksActivity() const { return m_tracked; }

    // ---- accounts ----------------------------------------------------------
    QList<UserSummary> users();
    QList<DeviceSession> devices(qint64 userId);
    QList<StoredRecord> records(qint64 userId, bool withDeleted = false);

    /*! The calendars of the account, out of its records and the shares. */
    QList<CalendarSummary> calendars(qint64 userId, const QList<StoredRecord> &records);

    /*!
     * Gives the account a new password and signs every device out of it, the
     * way --set-password does: the old sessions knew the old password.
     */
    bool setPassword(qint64 userId, const QString &password, QString *error);

    // ---- statistics ----------------------------------------------------------
    ServerTotals totals(const QDateTime &now = QDateTime::currentDateTime());

    /*! The last @p days days up to @p today, oldest first. */
    QList<DayActivity> history(int days, const QDate &today = QDate::currentDate());

    StorageUse storage();

private:
    int count(const QString &statement, const QVariantMap &values = {});

    QSqlDatabase m_db;
    QString m_connectionName;
    QString m_path;
    QString m_lastError;
    int m_schemaVersion = 0;
    bool m_tracked = false;
};
