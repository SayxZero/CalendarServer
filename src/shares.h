#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QSqlDatabase>
#include <QString>

/*! What a member may do with a shared calendar. */
enum class ShareAccess {
    Read = 0,
    Edit = 1,
};

/*! How the owner hands access out; see model/share.h on the planner side. */
enum class ShareMode {
    Read = 0,
    Edit = 1,
    Custom = 2,
};

/*! The enums as they travel in JSON; the planner spells them the same way. */
QString shareModeKey(ShareMode mode);
ShareMode shareModeFromKey(const QString &key);
QString shareAccessKey(ShareAccess access);
ShareAccess shareAccessFromKey(const QString &key);

struct ShareMemberInfo
{
    qint64 userId = 0;
    QString login;
    QString displayName;
    ShareAccess access = ShareAccess::Read;
    QString twinId;        //!< the id this member gave the calendar in their planner
    QDateTime joinedAt;
};

/*!
 * One calendar the owner handed out. The calendar itself stays a record of the
 * owner like any other; this is only who may see it and what they may do.
 */
struct ShareInfo
{
    QString id;
    qint64 ownerId = 0;
    QString ownerLogin;
    QString ownerDisplayName;
    QString calendarId;
    ShareMode mode = ShareMode::Custom;
    bool enabled = false;
    QList<ShareMemberInfo> members;

    bool isValid() const { return !id.isEmpty(); }
};

/*! An invitation. Only the hash of the token is stored, as with session tokens. */
struct ShareLinkInfo
{
    QString shareId;
    bool singleUse = true;
    qint64 expiresAt = 0; //!< ms since the epoch, 0 when the link never expires
    bool spent = false;
};

/*!
 * Why a change could not be applied to a shared calendar. The planner turns it
 * into a question for the user, so every reason has to be one it can answer.
 */
struct ShareConflict
{
    QString kind;
    QString id;
    QString calendarId; //!< as the device that sent the change spells it
    QString reason;     //!< "deleted", "read_only", "gone"
    QByteArray data;    //!< what the device tried to store, so it can be restored
};

/*!
 * The shared half of the server: who let whom into which calendar, the links
 * that hand access out, and the routing of one change into the record sets of
 * everybody who shares the calendar it belongs to.
 *
 * This is the one place where the server looks inside a record. Everywhere else
 * a record is opaque JSON, but a shared record has to have its "calendarId"
 * rewritten on the way: the owner and every member call the same calendar by
 * an id of their own, because the twin is a calendar of their own planner.
 */
class ShareStore
{
public:
    explicit ShareStore(QSqlDatabase db);

    bool applySchema(QString *error);

    // ---- what the owner set up --------------------------------------------
    ShareInfo share(const QString &shareId);
    ShareInfo shareOf(qint64 ownerId, const QString &calendarId);

    /*! Creates the share or changes it; an empty calendar id is refused. */
    ShareInfo configure(qint64 ownerId, const QString &calendarId, bool enabled, ShareMode mode,
                        QString *error);

    /*!
     * Raises or lowers one member. Only a share in the custom mode has members
     * of different rights; the other modes keep everybody at the same one.
     */
    bool setMemberAccess(qint64 ownerId, const QString &calendarId, const QString &login,
                         ShareAccess access, QString *error);

    /*! The owner shows a member the door, or a member leaves on their own. */
    bool removeMember(const QString &shareId, qint64 memberId, QString *error);

    /*! The share a member reaches through their own twin calendar. */
    ShareInfo shareOfTwin(qint64 memberId, const QString &twinId);

    /*! Every share this user takes part in, as owner or as member. */
    QList<ShareInfo> sharesOf(qint64 userId);

    // ---- links -------------------------------------------------------------
    /*! Returns the plain token; only its hash is kept. */
    QString createLink(qint64 ownerId, const QString &calendarId, bool singleUse,
                       qint64 lifeSeconds, QString *error);

    /*! The share a link leads to, with why it does not work in @p error. */
    ShareInfo resolveLink(const QString &token, QString *error);

    /*! Marks a single use link as spent, once somebody walked through it. */
    void spendLink(const QString &token);

    // ---- joining -----------------------------------------------------------
    /*!
     * Lets @p memberId into the share behind @p token and gives the member a
     * copy of everything the calendar holds, written as records of their own
     * with @p twinId as the calendar. Returns the share on success.
     */
    ShareInfo join(const QString &token, qint64 memberId, const QString &twinId, qint64 *revision,
                   QString *error);

    /*!
     * Takes the copies of a shared calendar out of a member's records, leaving
     * tombstones so that their devices delete them too.
     */
    bool dropCopies(qint64 memberId, const QString &twinId, qint64 *revision);

    /*!
     * An account goes away: the members of its calendars lose their copies
     * (tombstones, so that their devices drop them too), and its shares, their
     * links and its own memberships go. The records of the account itself are
     * the caller's to delete. Runs outside a transaction of the caller's.
     */
    bool forgetUser(qint64 userId);

    // ---- routing -----------------------------------------------------------
    /*!
     * Where one change has to go besides the record set it arrived in. Reads
     * the calendar out of @p data (or out of the stored record for a deletion)
     * and answers whether the sender may write it at all.
     */
    struct Route
    {
        bool shared = false;     //!< the record belongs to a shared calendar
        bool allowed = true;     //!< the sender may change it
        ShareInfo share;
        bool senderIsOwner = false;
        QString senderCalendarId; //!< how the sender calls the calendar
    };
    Route routeFor(qint64 senderId, const QString &kind, const QByteArray &data,
                   const QString &storedCalendarId);

    /*!
     * Writes one record into the set of @p userId, giving it the next revision
     * of that user: a copy of a shared record travels to a device by the
     * ordinary sync, with no protocol of its own.
     */
    bool putCopy(qint64 userId, const QString &kind, const QString &id, const QByteArray &data,
                 bool deleted, qint64 modified, const QString &device);

    /*!
     * Hands one change to everybody who shares the calendar but the sender:
     * the owner keeps the original, every member gets it with their own
     * calendar id. Returns how many sets were written, -1 on failure.
     */
    int fanOut(const ShareInfo &share, qint64 senderId, const QString &kind, const QString &id,
               const QByteArray &data, bool deleted, qint64 modified, const QString &device);

    /*! The calendar id a record names, empty when it names none. */
    static QString calendarOf(const QByteArray &data);

    /*! The same record with its calendar rewritten to @p calendarId. */
    static QByteArray withCalendar(const QByteArray &data, const QString &calendarId);

    /*! Whether a record of this kind lives in a calendar and therefore travels. */
    static bool travels(const QString &kind);

    QString lastError() const { return m_lastError; }

private:
    /*!
     * Writes @p access into the member's own twin calendar record, so that
     * their planner learns what it may now do with it. The right is kept in
     * share_members, but the planner on the other side only ever sees its own
     * records - that is how it hears about this one.
     */
    bool publishTwinAccess(qint64 memberId, const QString &twinId, ShareAccess access);

    ShareInfo readShare(const QString &shareId, bool withMembers = true);
    void loadMembers(ShareInfo *share);
    static QByteArray hashToken(const QString &token);

    QSqlDatabase m_db;
    QString m_lastError;
};
