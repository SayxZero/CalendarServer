#include "shares.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

QString shareAccessKey(ShareAccess access)
{
    return access == ShareAccess::Edit ? QStringLiteral("edit") : QStringLiteral("read");
}

ShareAccess shareAccessFromKey(const QString &key)
{
    return key == QLatin1String("edit") ? ShareAccess::Edit : ShareAccess::Read;
}

QString shareModeKey(ShareMode mode)
{
    switch (mode) {
    case ShareMode::Read:
        return QStringLiteral("read");
    case ShareMode::Edit:
        return QStringLiteral("edit");
    case ShareMode::Custom:
        break;
    }
    return QStringLiteral("custom");
}

ShareMode shareModeFromKey(const QString &key)
{
    if (key == QLatin1String("read"))
        return ShareMode::Read;
    if (key == QLatin1String("edit"))
        return ShareMode::Edit;
    return ShareMode::Custom;
}

namespace {

QString accessKey(ShareAccess access) { return shareAccessKey(access); }
ShareAccess accessFromKey(const QString &key) { return shareAccessFromKey(key); }
QString modeKey(ShareMode mode) { return shareModeKey(mode); }
ShareMode modeFromKey(const QString &key) { return shareModeFromKey(key); }

/*! The right a member gets the moment a link of this mode is accepted. */
ShareAccess initialAccess(ShareMode mode)
{
    return mode == ShareMode::Edit ? ShareAccess::Edit : ShareAccess::Read;
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

} // namespace

ShareStore::ShareStore(QSqlDatabase db)
    : m_db(std::move(db))
{
}

bool ShareStore::applySchema(QString *error)
{
    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS shares ("
                       "id TEXT PRIMARY KEY,"
                       "owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
                       "calendar_id TEXT NOT NULL,"
                       "mode TEXT NOT NULL,"
                       "enabled INTEGER NOT NULL DEFAULT 1,"
                       "created_at TEXT NOT NULL,"
                       "UNIQUE (owner_id, calendar_id))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS share_members ("
                       "share_id TEXT NOT NULL REFERENCES shares(id) ON DELETE CASCADE,"
                       "user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
                       "access TEXT NOT NULL,"
                       "twin_id TEXT NOT NULL,"
                       "joined_at TEXT NOT NULL,"
                       "PRIMARY KEY (share_id, user_id))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS share_links ("
                       "token_hash BLOB PRIMARY KEY,"
                       "share_id TEXT NOT NULL REFERENCES shares(id) ON DELETE CASCADE,"
                       "single_use INTEGER NOT NULL DEFAULT 1,"
                       "expires_at INTEGER NOT NULL DEFAULT 0,"
                       "spent INTEGER NOT NULL DEFAULT 0,"
                       "created_at TEXT NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_share_members_user"
                       " ON share_members(user_id, twin_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_shares_owner ON shares(owner_id)"),
    };
    for (const QString &statement : statements) {
        QSqlQuery query(m_db);
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            if (error)
                *error = m_lastError;
            return false;
        }
    }
    return true;
}

bool ShareStore::travels(const QString &kind)
{
    return kind == QLatin1String("event") || kind == QLatin1String("task")
           || kind == QLatin1String("completion");
}

QString ShareStore::calendarOf(const QByteArray &data)
{
    if (data.isEmpty())
        return QString();
    const QJsonObject object = QJsonDocument::fromJson(data).object();
    return object.value(QStringLiteral("calendarId")).toString();
}

QByteArray ShareStore::withCalendar(const QByteArray &data, const QString &calendarId)
{
    if (data.isEmpty())
        return data;
    QJsonObject object = QJsonDocument::fromJson(data).object();
    if (object.isEmpty())
        return data;
    object.insert(QStringLiteral("calendarId"), calendarId);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray ShareStore::hashToken(const QString &token)
{
    return QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256);
}

ShareInfo ShareStore::readShare(const QString &shareId, bool withMembers)
{
    ShareInfo info;
    if (shareId.isEmpty())
        return info;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT s.id, s.owner_id, s.calendar_id, s.mode, s.enabled, u.login, u.display_name"
        " FROM shares s JOIN users u ON u.id = s.owner_id WHERE s.id = :id"));
    query.bindValue(QStringLiteral(":id"), shareId);
    if (!query.exec() || !query.next())
        return info;

    info.id = query.value(0).toString();
    info.ownerId = query.value(1).toLongLong();
    info.calendarId = query.value(2).toString();
    info.mode = modeFromKey(query.value(3).toString());
    info.enabled = query.value(4).toBool();
    info.ownerLogin = query.value(5).toString();
    info.ownerDisplayName = query.value(6).toString();
    if (withMembers)
        loadMembers(&info);
    return info;
}

void ShareStore::loadMembers(ShareInfo *share)
{
    if (!share || share->id.isEmpty())
        return;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT m.user_id, m.access, m.twin_id, m.joined_at, u.login, u.display_name"
        " FROM share_members m JOIN users u ON u.id = m.user_id"
        " WHERE m.share_id = :id ORDER BY m.joined_at"));
    query.bindValue(QStringLiteral(":id"), share->id);
    if (!query.exec())
        return;
    while (query.next()) {
        ShareMemberInfo member;
        member.userId = query.value(0).toLongLong();
        member.access = accessFromKey(query.value(1).toString());
        member.twinId = query.value(2).toString();
        member.joinedAt = QDateTime::fromString(query.value(3).toString(), Qt::ISODate);
        member.login = query.value(4).toString();
        member.displayName = query.value(5).toString();
        share->members.append(member);
    }
}

ShareInfo ShareStore::share(const QString &shareId)
{
    return readShare(shareId);
}

ShareInfo ShareStore::shareOf(qint64 ownerId, const QString &calendarId)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT id FROM shares WHERE owner_id = :owner AND calendar_id = :calendar"));
    query.bindValue(QStringLiteral(":owner"), ownerId);
    query.bindValue(QStringLiteral(":calendar"), calendarId);
    if (!query.exec() || !query.next())
        return ShareInfo();
    return readShare(query.value(0).toString());
}

ShareInfo ShareStore::shareOfTwin(qint64 memberId, const QString &twinId)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT share_id FROM share_members WHERE user_id = :user AND twin_id = :twin"));
    query.bindValue(QStringLiteral(":user"), memberId);
    query.bindValue(QStringLiteral(":twin"), twinId);
    if (!query.exec() || !query.next())
        return ShareInfo();
    return readShare(query.value(0).toString());
}

QList<ShareInfo> ShareStore::sharesOf(qint64 userId)
{
    QList<ShareInfo> result;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT id FROM shares WHERE owner_id = :user"
        " UNION SELECT share_id FROM share_members WHERE user_id = :user"));
    query.bindValue(QStringLiteral(":user"), userId);
    if (!query.exec())
        return result;
    QStringList ids;
    while (query.next())
        ids << query.value(0).toString();
    for (const QString &id : ids) {
        const ShareInfo info = readShare(id);
        if (info.isValid())
            result.append(info);
    }
    return result;
}

ShareInfo ShareStore::configure(qint64 ownerId, const QString &calendarId, bool enabled,
                                ShareMode mode, QString *error)
{
    if (calendarId.isEmpty()) {
        if (error)
            *error = QStringLiteral("Не указан календарь");
        return ShareInfo();
    }

    ShareInfo existing = shareOf(ownerId, calendarId);
    if (!existing.isValid()) {
        QSqlQuery insert(m_db);
        insert.prepare(QStringLiteral(
            "INSERT INTO shares (id, owner_id, calendar_id, mode, enabled, created_at)"
            " VALUES (:id, :owner, :calendar, :mode, :enabled, :created)"));
        const QString id = newId();
        insert.bindValue(QStringLiteral(":id"), id);
        insert.bindValue(QStringLiteral(":owner"), ownerId);
        insert.bindValue(QStringLiteral(":calendar"), calendarId);
        insert.bindValue(QStringLiteral(":mode"), modeKey(mode));
        insert.bindValue(QStringLiteral(":enabled"), enabled ? 1 : 0);
        insert.bindValue(QStringLiteral(":created"),
                         QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        if (!insert.exec()) {
            m_lastError = insert.lastError().text();
            if (error)
                *error = m_lastError;
            return ShareInfo();
        }
        return readShare(id);
    }

    QSqlQuery update(m_db);
    update.prepare(QStringLiteral(
        "UPDATE shares SET mode = :mode, enabled = :enabled WHERE id = :id"));
    update.bindValue(QStringLiteral(":mode"), modeKey(mode));
    update.bindValue(QStringLiteral(":enabled"), enabled ? 1 : 0);
    update.bindValue(QStringLiteral(":id"), existing.id);
    if (!update.exec()) {
        m_lastError = update.lastError().text();
        if (error)
            *error = m_lastError;
        return ShareInfo();
    }

    // Read and edit hold everybody at the same right; only the custom mode
    // keeps what the owner set for single members.
    if (mode != ShareMode::Custom) {
        QSqlQuery level(m_db);
        level.prepare(QStringLiteral(
            "UPDATE share_members SET access = :access WHERE share_id = :id"));
        level.bindValue(QStringLiteral(":access"), accessKey(initialAccess(mode)));
        level.bindValue(QStringLiteral(":id"), existing.id);
        if (!level.exec()) {
            m_lastError = level.lastError().text();
            if (error)
                *error = m_lastError;
            return ShareInfo();
        }

        // Same reason as in setMemberAccess(): every member hears about their
        // right through their own twin calendar and nowhere else.
        const ShareInfo fresh = readShare(existing.id);
        for (const ShareMemberInfo &member : fresh.members) {
            if (!publishTwinAccess(member.userId, member.twinId, initialAccess(mode))) {
                if (error)
                    *error = m_lastError;
                return ShareInfo();
            }
        }
    }
    return readShare(existing.id);
}

bool ShareStore::setMemberAccess(qint64 ownerId, const QString &calendarId, const QString &login,
                                 ShareAccess access, QString *error)
{
    const ShareInfo info = shareOf(ownerId, calendarId);
    if (!info.isValid()) {
        if (error)
            *error = QStringLiteral("Этот календарь никому не открыт");
        return false;
    }
    if (info.mode != ShareMode::Custom) {
        if (error) {
            *error = QStringLiteral(
                "Права участников меняются только в настраиваемом типе доступа");
        }
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE share_members SET access = :access WHERE share_id = :share AND user_id ="
        " (SELECT id FROM users WHERE login_key = :login)"));
    query.bindValue(QStringLiteral(":access"), accessKey(access));
    query.bindValue(QStringLiteral(":share"), info.id);
    query.bindValue(QStringLiteral(":login"), login.toLower());
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        if (error)
            *error = m_lastError;
        return false;
    }
    if (query.numRowsAffected() <= 0) {
        if (error)
            *error = QStringLiteral("Участник «%1» не найден").arg(login);
        return false;
    }

    // The member's planner reads the access off its own twin calendar, so the
    // new right has to travel there; changing share_members alone leaves that
    // planner believing what it was told when it joined.
    const ShareInfo fresh = readShare(info.id);
    for (const ShareMemberInfo &member : fresh.members) {
        if (member.login.compare(login, Qt::CaseInsensitive) != 0)
            continue;
        if (!publishTwinAccess(member.userId, member.twinId, access)) {
            if (error)
                *error = m_lastError;
            return false;
        }
        break;
    }
    return true;
}

bool ShareStore::publishTwinAccess(qint64 memberId, const QString &twinId, ShareAccess access)
{
    QSqlQuery read(m_db);
    read.prepare(QStringLiteral(
        "SELECT data FROM records WHERE user_id = :user AND kind = 'calendar' AND id = :id"
        " AND deleted = 0"));
    read.bindValue(QStringLiteral(":user"), memberId);
    read.bindValue(QStringLiteral(":id"), twinId);
    if (!read.exec()) {
        m_lastError = read.lastError().text();
        return false;
    }
    if (!read.next()) {
        // Joined but the twin has not reached the server yet; it will carry the
        // access it was created with, and the next change of it travels here.
        return true;
    }

    QJsonObject object = QJsonDocument::fromJson(read.value(0).toByteArray()).object();
    if (object.isEmpty())
        return true;
    if (object.value(QStringLiteral("access")).toString() == accessKey(access))
        return true;
    object.insert(QStringLiteral("access"), accessKey(access));

    // Newer than whatever the member's devices hold, so "the later edit wins"
    // lets it through rather than treating it as a stale copy.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    return putCopy(memberId, QStringLiteral("calendar"), twinId,
                   QJsonDocument(object).toJson(QJsonDocument::Compact), false, now,
                   QStringLiteral("share"));
}

bool ShareStore::removeMember(const QString &shareId, qint64 memberId, QString *error)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "DELETE FROM share_members WHERE share_id = :share AND user_id = :user"));
    query.bindValue(QStringLiteral(":share"), shareId);
    query.bindValue(QStringLiteral(":user"), memberId);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        if (error)
            *error = m_lastError;
        return false;
    }
    return true;
}

QString ShareStore::createLink(qint64 ownerId, const QString &calendarId, bool singleUse,
                               qint64 lifeSeconds, QString *error)
{
    const ShareInfo info = shareOf(ownerId, calendarId);
    if (!info.isValid() || !info.enabled) {
        if (error)
            *error = QStringLiteral("Совместный доступ к этому календарю выключен");
        return QString();
    }

    // The token is the whole secret of the link, so it comes from the system
    // generator and never from the id of anything.
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator *generator = QRandomGenerator::system();
    for (int i = 0; i < raw.size(); ++i)
        raw[i] = char(generator->bounded(256));
    const QString token = QString::fromLatin1(raw.toBase64(QByteArray::Base64UrlEncoding
                                                           | QByteArray::OmitTrailingEquals));

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO share_links (token_hash, share_id, single_use, expires_at, spent, created_at)"
        " VALUES (:hash, :share, :single, :expires, 0, :created)"));
    query.bindValue(QStringLiteral(":hash"), hashToken(token));
    query.bindValue(QStringLiteral(":share"), info.id);
    query.bindValue(QStringLiteral(":single"), singleUse ? 1 : 0);
    query.bindValue(QStringLiteral(":expires"),
                    lifeSeconds > 0
                        ? QDateTime::currentMSecsSinceEpoch() + lifeSeconds * 1000
                        : 0);
    query.bindValue(QStringLiteral(":created"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        if (error)
            *error = m_lastError;
        return QString();
    }
    return token;
}

ShareInfo ShareStore::resolveLink(const QString &token, QString *error)
{
    if (token.isEmpty()) {
        if (error)
            *error = QStringLiteral("Ссылка пуста");
        return ShareInfo();
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT share_id, single_use, expires_at, spent FROM share_links WHERE token_hash = :hash"));
    query.bindValue(QStringLiteral(":hash"), hashToken(token));
    if (!query.exec() || !query.next()) {
        if (error)
            *error = QStringLiteral("Такой ссылки нет");
        return ShareInfo();
    }

    const QString shareId = query.value(0).toString();
    const bool singleUse = query.value(1).toBool();
    const qint64 expiresAt = query.value(2).toLongLong();
    const bool spent = query.value(3).toBool();

    if (singleUse && spent) {
        if (error)
            *error = QStringLiteral("Эта ссылка одноразовая и уже использована");
        return ShareInfo();
    }
    if (expiresAt > 0 && QDateTime::currentMSecsSinceEpoch() > expiresAt) {
        if (error)
            *error = QStringLiteral("Срок действия ссылки истёк");
        return ShareInfo();
    }

    const ShareInfo info = readShare(shareId);
    if (!info.isValid() || !info.enabled) {
        if (error)
            *error = QStringLiteral("Владелец закрыл совместный доступ к этому календарю");
        return ShareInfo();
    }
    return info;
}

void ShareStore::spendLink(const QString &token)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE share_links SET spent = 1 WHERE token_hash = :hash"));
    query.bindValue(QStringLiteral(":hash"), hashToken(token));
    query.exec();
}

ShareStore::Route ShareStore::routeFor(qint64 senderId, const QString &kind,
                                       const QByteArray &data, const QString &storedCalendarId)
{
    Route route;
    if (!travels(kind))
        return route;

    // A deletion carries no data, so the calendar of the stored record decides.
    QString calendarId = calendarOf(data);
    if (calendarId.isEmpty())
        calendarId = storedCalendarId;
    if (calendarId.isEmpty())
        return route;

    route.senderCalendarId = calendarId;

    ShareInfo info = shareOf(senderId, calendarId);
    if (info.isValid()) {
        route.shared = info.enabled && !info.members.isEmpty();
        route.share = info;
        route.senderIsOwner = true;
        route.allowed = true;
        return route;
    }

    info = shareOfTwin(senderId, calendarId);
    if (!info.isValid())
        return route;

    route.shared = true;
    route.share = info;
    route.senderIsOwner = false;

    // A member may only write with the right the owner gave.
    route.allowed = false;
    for (const ShareMemberInfo &member : info.members) {
        if (member.userId == senderId)
            route.allowed = info.enabled && member.access == ShareAccess::Edit;
    }
    return route;
}

// ---------------------------------------------------------------- copies ----

namespace {

/*! The revision counter of one user, the cursor its devices sync against. */
qint64 revisionOf(QSqlDatabase &db, qint64 userId)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT revision FROM users WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), userId);
    if (!query.exec() || !query.next())
        return 0;
    return query.value(0).toLongLong();
}

bool setRevision(QSqlDatabase &db, qint64 userId, qint64 revision)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE users SET revision = :revision WHERE id = :id"));
    query.bindValue(QStringLiteral(":revision"), revision);
    query.bindValue(QStringLiteral(":id"), userId);
    return query.exec();
}

} // namespace

bool ShareStore::putCopy(qint64 userId, const QString &kind, const QString &id,
                         const QByteArray &data, bool deleted, qint64 modified,
                         const QString &device)
{
    const qint64 revision = revisionOf(m_db, userId) + 1;
    QSqlQuery write(m_db);
    write.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO records (user_id, kind, id, data, deleted, modified, revision,"
        " device) VALUES (:user, :kind, :id, :data, :deleted, :modified, :revision, :device)"));
    write.bindValue(QStringLiteral(":user"), userId);
    write.bindValue(QStringLiteral(":kind"), kind);
    write.bindValue(QStringLiteral(":id"), id);
    write.bindValue(QStringLiteral(":data"), deleted ? QByteArray() : data);
    write.bindValue(QStringLiteral(":deleted"), deleted ? 1 : 0);
    write.bindValue(QStringLiteral(":modified"), modified);
    write.bindValue(QStringLiteral(":revision"), revision);
    write.bindValue(QStringLiteral(":device"), device.left(200));
    if (!write.exec()) {
        m_lastError = write.lastError().text();
        return false;
    }
    if (!setRevision(m_db, userId, revision)) {
        m_lastError = QStringLiteral("не удалось обновить курсор пользователя");
        return false;
    }
    return true;
}

int ShareStore::fanOut(const ShareInfo &share, qint64 senderId, const QString &kind,
                       const QString &id, const QByteArray &data, bool deleted, qint64 modified,
                       const QString &device)
{
    int written = 0;

    // The owner always keeps the original: a member's change reaches everybody
    // else through it, so it is written first.
    if (senderId != share.ownerId) {
        if (!putCopy(share.ownerId, kind, id, withCalendar(data, share.calendarId), deleted,
                     modified, device)) {
            return -1;
        }
        ++written;
    }

    for (const ShareMemberInfo &member : share.members) {
        if (member.userId == senderId)
            continue;
        if (!putCopy(member.userId, kind, id, withCalendar(data, member.twinId), deleted, modified,
                     device)) {
            return -1;
        }
        ++written;
    }
    return written;
}

ShareInfo ShareStore::join(const QString &token, qint64 memberId, const QString &twinId,
                           qint64 *revision, QString *error)
{
    const ShareInfo info = resolveLink(token, error);
    if (!info.isValid())
        return ShareInfo();

    if (info.ownerId == memberId) {
        if (error)
            *error = QStringLiteral("Это ваш собственный календарь");
        return ShareInfo();
    }
    if (twinId.isEmpty()) {
        if (error)
            *error = QStringLiteral("Не указан календарь, в который добавить");
        return ShareInfo();
    }

    // Walking through a second link to the same calendar keeps the twin that is
    // already in the planner instead of making another one.
    QString effectiveTwin = twinId;
    for (const ShareMemberInfo &member : info.members) {
        if (member.userId == memberId)
            effectiveTwin = member.twinId;
    }

    QSqlQuery insert(m_db);
    insert.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO share_members (share_id, user_id, access, twin_id, joined_at)"
        " VALUES (:share, :user, :access, :twin, :joined)"));
    insert.bindValue(QStringLiteral(":share"), info.id);
    insert.bindValue(QStringLiteral(":user"), memberId);
    insert.bindValue(QStringLiteral(":access"), accessKey(initialAccess(info.mode)));
    insert.bindValue(QStringLiteral(":twin"), effectiveTwin);
    insert.bindValue(QStringLiteral(":joined"),
                     QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    if (!insert.exec()) {
        m_lastError = insert.lastError().text();
        if (error)
            *error = m_lastError;
        return ShareInfo();
    }

    // Everything the calendar holds right now becomes records of the member,
    // so the next ordinary sync brings the whole calendar down to the device.
    QSqlQuery read(m_db);
    read.prepare(QStringLiteral(
        "SELECT kind, id, data, modified FROM records"
        " WHERE user_id = :owner AND deleted = 0 AND kind IN ('event', 'task', 'completion')"));
    read.bindValue(QStringLiteral(":owner"), info.ownerId);
    if (!read.exec()) {
        m_lastError = read.lastError().text();
        if (error)
            *error = m_lastError;
        return ShareInfo();
    }

    struct Copy
    {
        QString kind;
        QString id;
        QByteArray data;
        qint64 modified;
    };
    QList<Copy> copies;
    while (read.next()) {
        const QByteArray data = read.value(2).toByteArray();
        if (calendarOf(data) != info.calendarId)
            continue;
        copies.append({ read.value(0).toString(), read.value(1).toString(), data,
                        read.value(3).toLongLong() });
    }

    for (const Copy &copy : copies) {
        if (!putCopy(memberId, copy.kind, copy.id, withCalendar(copy.data, effectiveTwin), false,
                     copy.modified, QStringLiteral("share"))) {
            if (error)
                *error = m_lastError;
            return ShareInfo();
        }
    }

    if (revision)
        *revision = revisionOf(m_db, memberId);

    return readShare(info.id);
}

bool ShareStore::forgetUser(qint64 userId)
{
    for (const ShareInfo &share : sharesOf(userId)) {
        if (share.ownerId != userId)
            continue;
        // Somebody else's calendar would otherwise stay on every member's
        // device as it was on the day its owner was deleted.
        for (const ShareMemberInfo &member : share.members) {
            if (member.userId != userId && !dropCopies(member.userId, member.twinId, nullptr))
                return false;
        }
    }

    const QStringList statements = {
        QStringLiteral("DELETE FROM share_links WHERE share_id IN"
                       " (SELECT id FROM shares WHERE owner_id = :user)"),
        QStringLiteral("DELETE FROM share_members WHERE share_id IN"
                       " (SELECT id FROM shares WHERE owner_id = :user)"),
        QStringLiteral("DELETE FROM share_members WHERE user_id = :user"),
        QStringLiteral("DELETE FROM shares WHERE owner_id = :user"),
    };
    for (const QString &statement : statements) {
        QSqlQuery query(m_db);
        query.prepare(statement);
        query.bindValue(QStringLiteral(":user"), userId);
        if (!query.exec()) {
            m_lastError = query.lastError().text();
            return false;
        }
    }
    return true;
}

bool ShareStore::dropCopies(qint64 memberId, const QString &twinId, qint64 *revision)
{
    QSqlQuery read(m_db);
    read.prepare(QStringLiteral(
        "SELECT kind, id, data FROM records"
        " WHERE user_id = :user AND deleted = 0 AND kind IN ('event', 'task', 'completion')"));
    read.bindValue(QStringLiteral(":user"), memberId);
    if (!read.exec()) {
        m_lastError = read.lastError().text();
        return false;
    }

    QList<QPair<QString, QString>> doomed;
    while (read.next()) {
        if (calendarOf(read.value(2).toByteArray()) != twinId)
            continue;
        doomed.append({ read.value(0).toString(), read.value(1).toString() });
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const auto &record : doomed) {
        // Tombstones rather than plain deletes: the devices of this member have
        // the records and would keep them otherwise.
        if (!putCopy(memberId, record.first, record.second, QByteArray(), true, now,
                     QStringLiteral("share"))) {
            return false;
        }
    }
    if (revision)
        *revision = revisionOf(m_db, memberId);
    return true;
}
