#include "admindb.h"

#include "api.h"
#include "config.h"
#include "records.h"

#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include <algorithm>

namespace {

QDateTime stamp(const QVariant &value)
{
    return QDateTime::fromString(value.toString(), Qt::ISODate);
}

QString utcText(const QDateTime &moment)
{
    return moment.toUTC().toString(Qt::ISODate);
}

QString dayText(const QDate &day)
{
    return day.toString(Qt::ISODate);
}

} // namespace

AdminDb::AdminDb()
    : m_connectionName(QStringLiteral("admin-%1").arg(QUuid::createUuid().toString(QUuid::Id128)))
{
}

AdminDb::~AdminDb()
{
    close();
}

bool AdminDb::open(const QString &path, QString *error)
{
    close();
    const auto fail = [this, error](const QString &message) {
        m_lastError = message;
        if (error)
            *error = message;
        close();
        return false;
    };

    // QSQLITE would create an empty file in place of a missing one, and the
    // server would then start on it as on its own.
    if (!QFileInfo(path).isFile())
        return fail(QStringLiteral("базы ещё нет: сервер создаст её при первом запуске"));

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(path);
    m_db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!m_db.open())
        return fail(m_db.lastError().text());
    m_path = path;

    QSqlQuery query(m_db);
    QSet<QString> tables;
    if (!query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type = 'table'")))
        return fail(QStringLiteral("это не база SQLite: %1").arg(query.lastError().text()));
    while (query.next())
        tables.insert(query.value(0).toString());
    if (!tables.contains(QStringLiteral("users")) || !tables.contains(QStringLiteral("records")))
        return fail(QStringLiteral("это не база сервера синхронизации"));
    m_tracked = tables.contains(QStringLiteral("activity"));
    if (query.exec(QStringLiteral("PRAGMA user_version")) && query.next())
        m_schemaVersion = query.value(0).toInt();
    return true;
}

void AdminDb::close()
{
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase();
    if (QSqlDatabase::contains(m_connectionName))
        QSqlDatabase::removeDatabase(m_connectionName);
    m_path.clear();
    m_schemaVersion = 0;
    m_tracked = false;
}

int AdminDb::count(const QString &statement, const QVariantMap &values)
{
    QSqlQuery query(m_db);
    query.prepare(statement);
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        query.bindValue(it.key(), it.value());
    if (query.exec() && query.next())
        return query.value(0).toInt();
    m_lastError = query.lastError().text();
    return 0;
}

// ------------------------------------------------------------------ accounts

QList<UserSummary> AdminDb::users()
{
    QList<UserSummary> result;
    QHash<qint64, int> index;

    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral(
            "SELECT id, login, display_name, created_at, revision FROM users ORDER BY login_key"))) {
        m_lastError = query.lastError().text();
        return result;
    }
    while (query.next()) {
        UserSummary user;
        user.id = query.value(0).toLongLong();
        user.login = query.value(1).toString();
        user.displayName = query.value(2).toString();
        user.createdAt = stamp(query.value(3));
        user.revision = query.value(4).toLongLong();
        index.insert(user.id, int(result.size()));
        result.append(user);
    }

    if (query.exec(QStringLiteral(
            "SELECT user_id, kind, COUNT(*), SUM(LENGTH(data)) FROM records WHERE deleted = 0"
            " GROUP BY user_id, kind"))) {
        while (query.next()) {
            const auto at = index.constFind(query.value(0).toLongLong());
            if (at == index.cend())
                continue;
            UserSummary &user = result[*at];
            const QString kind = query.value(1).toString();
            const int number = query.value(2).toInt();
            user.records += number;
            user.bytes += query.value(3).toLongLong();
            if (kind == QLatin1String("calendar"))
                user.calendars = number;
            else if (kind == QLatin1String("event"))
                user.events = number;
            else if (kind == QLatin1String("task"))
                user.tasks = number;
        }
    }

    if (query.exec(QStringLiteral(
            "SELECT user_id, COUNT(*), MAX(last_used_at) FROM tokens GROUP BY user_id"))) {
        while (query.next()) {
            const auto at = index.constFind(query.value(0).toLongLong());
            if (at == index.cend())
                continue;
            result[*at].devices = query.value(1).toInt();
            result[*at].lastSeen = stamp(query.value(2));
        }
    }

    if (m_tracked) {
        query.prepare(QStringLiteral(
            "SELECT user_id, MAX(last_at), SUM(CASE WHEN day >= :from THEN syncs ELSE 0 END)"
            " FROM activity GROUP BY user_id"));
        query.bindValue(QStringLiteral(":from"), dayText(QDate::currentDate().addDays(-29)));
        if (query.exec()) {
            while (query.next()) {
                const auto at = index.constFind(query.value(0).toLongLong());
                if (at == index.cend())
                    continue;
                result[*at].lastSync = stamp(query.value(1));
                result[*at].syncsMonth = query.value(2).toInt();
            }
        }
    }
    return result;
}

QList<DeviceSession> AdminDb::devices(qint64 userId)
{
    QList<DeviceSession> result;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT device, created_at, last_used_at FROM tokens"
                                 " WHERE user_id = :user ORDER BY last_used_at DESC"));
    query.bindValue(QStringLiteral(":user"), userId);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return result;
    }
    while (query.next()) {
        DeviceSession device;
        device.name = query.value(0).toString();
        device.createdAt = stamp(query.value(1));
        device.lastUsedAt = stamp(query.value(2));
        result.append(device);
    }
    return result;
}

QList<StoredRecord> AdminDb::records(qint64 userId, bool withDeleted)
{
    QList<StoredRecord> result;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT kind, id, deleted, modified, revision, device, data"
                                 " FROM records WHERE user_id = :user%1 ORDER BY kind, revision")
                      .arg(withDeleted ? QString() : QStringLiteral(" AND deleted = 0")));
    query.bindValue(QStringLiteral(":user"), userId);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return result;
    }
    while (query.next()) {
        StoredRecord record;
        record.kind = query.value(0).toString();
        record.id = query.value(1).toString();
        record.deleted = query.value(2).toBool();
        record.modified = query.value(3).toLongLong();
        record.revision = query.value(4).toLongLong();
        record.device = query.value(5).toString();
        const QByteArray data = query.value(6).toByteArray();
        record.size = data.size();
        record.data = QJsonDocument::fromJson(data).object();
        result.append(record);
    }
    return result;
}

QList<CalendarSummary> AdminDb::calendars(qint64 userId, const QList<StoredRecord> &records)
{
    QList<CalendarSummary> result;
    QHash<QString, int> index;

    // The shares this account takes part in: its own calendars opened to
    // others by their calendar id, and the calendars of others by the id of
    // the twin it keeps of them.
    QHash<QString, ShareInfo> opened;
    QHash<QString, QPair<ShareInfo, ShareMemberInfo>> twins;
    ShareStore shares(m_db);
    for (const ShareInfo &share : shares.sharesOf(userId)) {
        if (share.ownerId == userId) {
            opened.insert(share.calendarId, share);
            continue;
        }
        for (const ShareMemberInfo &member : share.members) {
            if (member.userId == userId)
                twins.insert(member.twinId, qMakePair(share, member));
        }
    }

    const auto fillShare = [&](CalendarSummary *calendar) {
        if (opened.contains(calendar->id)) {
            const ShareInfo share = opened.value(calendar->id);
            calendar->role = CalendarSummary::Role::Open;
            calendar->mode = share.mode;
            calendar->enabled = share.enabled;
            calendar->members = share.members;
        } else if (twins.contains(calendar->id)) {
            const auto pair = twins.value(calendar->id);
            calendar->role = CalendarSummary::Role::Twin;
            calendar->ownerLogin = pair.first.ownerLogin;
            calendar->ownerDisplayName = pair.first.ownerDisplayName;
            calendar->access = pair.second.access;
            // The name the owner gave it: the twin's own may differ.
            QSqlQuery origin(m_db);
            origin.prepare(QStringLiteral("SELECT data FROM records WHERE user_id = :owner"
                                          " AND kind = 'calendar' AND id = :id"));
            origin.bindValue(QStringLiteral(":owner"), pair.first.ownerId);
            origin.bindValue(QStringLiteral(":id"), pair.first.calendarId);
            if (origin.exec() && origin.next()) {
                calendar->originName = PlannerRecord::title(
                    QStringLiteral("calendar"),
                    QJsonDocument::fromJson(origin.value(0).toByteArray()).object());
            }
        }
    };

    for (const StoredRecord &record : records) {
        if (record.kind != QLatin1String("calendar") || record.deleted)
            continue;
        CalendarSummary calendar;
        calendar.id = record.id;
        calendar.name = PlannerRecord::title(record.kind, record.data);
        calendar.color = record.data.value(QStringLiteral("color")).toString();
        fillShare(&calendar);
        index.insert(calendar.id, int(result.size()));
        result.append(calendar);
    }
    // A twin whose calendar record the planner has not sent yet.
    for (auto it = twins.cbegin(); it != twins.cend(); ++it) {
        if (index.contains(it.key()))
            continue;
        CalendarSummary calendar;
        calendar.id = it.key();
        calendar.missing = true;
        fillShare(&calendar);
        index.insert(calendar.id, int(result.size()));
        result.append(calendar);
    }

    for (const StoredRecord &record : records) {
        if (record.deleted || record.kind == QLatin1String("calendar"))
            continue;
        const auto at = index.constFind(PlannerRecord::calendarId(record.data));
        if (at == index.cend())
            continue;
        CalendarSummary &calendar = result[*at];
        if (record.kind == QLatin1String("event"))
            ++calendar.events;
        else if (record.kind == QLatin1String("task"))
            ++calendar.tasks;
        else
            ++calendar.other;
    }
    return result;
}

bool AdminDb::setPassword(qint64 userId, const QString &password, QString *error)
{
    const auto fail = [this, error](const QString &message) {
        m_lastError = message;
        if (error)
            *error = message;
        return false;
    };
    const QString problem = Api::passwordProblem(password);
    if (!problem.isEmpty())
        return fail(problem);

    const QString hash = Api::hashPassword(password, ServerConfig().passwordIterations);
    if (!m_db.transaction())
        return fail(m_db.lastError().text());
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral("UPDATE users SET password_hash = :hash WHERE id = :id"));
    update.bindValue(QStringLiteral(":hash"), hash);
    update.bindValue(QStringLiteral(":id"), userId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        const QString message = update.lastError().isValid() ? update.lastError().text()
                                                             : QStringLiteral("нет такого аккаунта");
        m_db.rollback();
        return fail(message);
    }
    QSqlQuery signOut(m_db);
    signOut.prepare(QStringLiteral("DELETE FROM tokens WHERE user_id = :id"));
    signOut.bindValue(QStringLiteral(":id"), userId);
    if (!signOut.exec()) {
        const QString message = signOut.lastError().text();
        m_db.rollback();
        return fail(message);
    }
    if (!m_db.commit()) {
        const QString message = m_db.lastError().text();
        m_db.rollback();
        return fail(message);
    }
    return true;
}

// ---------------------------------------------------------------- statistics

ServerTotals AdminDb::totals(const QDateTime &now)
{
    ServerTotals totals;
    const QDate today = now.date();
    const QString month = utcText(now.addDays(-30));

    totals.accounts = count(QStringLiteral("SELECT COUNT(*) FROM users"));
    totals.newAccounts = count(QStringLiteral("SELECT COUNT(*) FROM users WHERE created_at >= :since"),
                               { { QStringLiteral(":since"), month } });
    totals.devices = count(QStringLiteral("SELECT COUNT(*) FROM tokens"));
    totals.devicesMonth = count(
        QStringLiteral("SELECT COUNT(*) FROM tokens WHERE last_used_at >= :since"),
        { { QStringLiteral(":since"), month } });

    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("SELECT kind, deleted, COUNT(*) FROM records GROUP BY kind, deleted"))) {
        QHash<QString, int> live;
        while (query.next()) {
            const int number = query.value(2).toInt();
            if (query.value(1).toBool()) {
                totals.tombstones += number;
            } else {
                totals.records += number;
                live[query.value(0).toString()] += number;
            }
        }
        for (const QString &kind : PlannerRecord::kinds()) {
            if (live.contains(kind))
                totals.recordsByKind.append(qMakePair(kind, live.take(kind)));
        }
        for (auto it = live.cbegin(); it != live.cend(); ++it)
            totals.recordsByKind.append(qMakePair(it.key(), it.value()));
    }

    // A base of version 1 has no shared calendars at all.
    totals.sharedCalendars = count(QStringLiteral("SELECT COUNT(*) FROM shares WHERE enabled = 1"));
    totals.shareMembers = count(QStringLiteral(
        "SELECT COUNT(*) FROM share_members m JOIN shares s ON s.id = m.share_id WHERE s.enabled = 1"));
    // A link of many uses is marked spent too, once somebody walked through it.
    totals.liveLinks = count(
        QStringLiteral("SELECT COUNT(*) FROM share_links l JOIN shares s ON s.id = l.share_id"
                       " WHERE s.enabled = 1 AND (l.single_use = 0 OR l.spent = 0)"
                       " AND (l.expires_at = 0 OR l.expires_at > :now)"),
        { { QStringLiteral(":now"), now.toMSecsSinceEpoch() } });

    totals.tracked = m_tracked;
    if (!m_tracked)
        return totals;

    if (query.exec(QStringLiteral("SELECT MIN(day) FROM activity")) && query.next())
        totals.trackedSince = QDate::fromString(query.value(0).toString(), Qt::ISODate);

    const auto active = [this](const QDateTime &since) {
        return count(QStringLiteral("SELECT COUNT(DISTINCT user_id) FROM activity WHERE last_at >= :since"),
                     { { QStringLiteral(":since"), utcText(since) } });
    };
    totals.activeDay = active(now.addDays(-1));
    totals.activeWeek = active(now.addDays(-7));
    totals.activeMonth = active(now.addDays(-30));

    const auto syncs = [this](const QDate &from) {
        return count(QStringLiteral("SELECT COALESCE(SUM(syncs), 0) FROM activity WHERE day >= :from"),
                     { { QStringLiteral(":from"), dayText(from) } });
    };
    totals.syncsDay = syncs(today);
    totals.syncsWeek = syncs(today.addDays(-6));
    totals.syncsMonth = syncs(today.addDays(-29));

    query.prepare(QStringLiteral("SELECT COALESCE(SUM(bytes_in), 0), COALESCE(SUM(bytes_out), 0),"
                                 " COALESCE(SUM(changes_in), 0) FROM activity WHERE day >= :from"));
    query.bindValue(QStringLiteral(":from"), dayText(today.addDays(-29)));
    if (query.exec() && query.next()) {
        totals.bytesInMonth = query.value(0).toLongLong();
        totals.bytesOutMonth = query.value(1).toLongLong();
        totals.changesInMonth = query.value(2).toLongLong();
    }
    return totals;
}

QList<DayActivity> AdminDb::history(int days, const QDate &today)
{
    QList<DayActivity> result;
    days = qMax(1, days);
    const QDate first = today.addDays(-(days - 1));
    // The month before the first day too: its weekly and monthly counts look back.
    const QDate from = first.addDays(-29);

    QHash<QDate, QSet<qint64>> seen;
    QHash<QDate, DayActivity> sums;
    if (m_tracked) {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral("SELECT user_id, day, syncs, bytes_in, bytes_out FROM activity"
                                     " WHERE day >= :from AND day <= :to"));
        query.bindValue(QStringLiteral(":from"), dayText(from));
        query.bindValue(QStringLiteral(":to"), dayText(today));
        if (query.exec()) {
            while (query.next()) {
                const QDate day = QDate::fromString(query.value(1).toString(), Qt::ISODate);
                seen[day].insert(query.value(0).toLongLong());
                DayActivity &sum = sums[day];
                sum.syncs += query.value(2).toInt();
                sum.bytesIn += query.value(3).toLongLong();
                sum.bytesOut += query.value(4).toLongLong();
            }
        }
    }

    // Accounts by the local day they were created on.
    QHash<QDate, int> created;
    int before = 0;
    QSqlQuery accounts(m_db);
    if (accounts.exec(QStringLiteral("SELECT created_at FROM users"))) {
        while (accounts.next()) {
            const QDate day = stamp(accounts.value(0)).toLocalTime().date();
            if (day < first)
                ++before;
            else
                ++created[day];
        }
    }

    const auto distinct = [&seen](const QDate &last, int span) {
        QSet<qint64> users;
        for (int back = 0; back < span; ++back)
            users.unite(seen.value(last.addDays(-back)));
        return int(users.size());
    };

    int total = before;
    for (QDate day = first; day <= today; day = day.addDays(1)) {
        DayActivity point = sums.value(day);
        point.day = day;
        point.daily = int(seen.value(day).size());
        point.weekly = distinct(day, 7);
        point.monthly = distinct(day, 30);
        point.newAccounts = created.value(day);
        total += point.newAccounts;
        point.accounts = total;
        result.append(point);
    }
    return result;
}

StorageUse AdminDb::storage()
{
    StorageUse use;
    use.file = QFileInfo(m_path).size();
    use.wal = QFileInfo(m_path + QStringLiteral("-wal")).size();
    use.shm = QFileInfo(m_path + QStringLiteral("-shm")).size();

    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("PRAGMA page_size")) && query.next())
        use.pageSize = query.value(0).toLongLong();
    if (query.exec(QStringLiteral("PRAGMA page_count")) && query.next())
        use.pages = query.value(0).toLongLong();
    if (query.exec(QStringLiteral("PRAGMA freelist_count")) && query.next())
        use.freePages = query.value(0).toLongLong();

    if (query.exec(QStringLiteral("SELECT kind, COUNT(*), COALESCE(SUM(LENGTH(data)), 0)"
                                  " FROM records WHERE deleted = 0 GROUP BY kind"))) {
        while (query.next()) {
            StorageUse::Kind kind;
            kind.kind = query.value(0).toString();
            kind.count = query.value(1).toInt();
            kind.bytes = query.value(2).toLongLong();
            use.payload += kind.bytes;
            use.byKind.append(kind);
        }
        std::sort(use.byKind.begin(), use.byKind.end(),
                  [](const StorageUse::Kind &a, const StorageUse::Kind &b) { return a.bytes > b.bytes; });
    }
    use.tombstones = count(QStringLiteral("SELECT COUNT(*) FROM records WHERE deleted = 1"));

    if (query.exec(QStringLiteral(
            "SELECT u.login, COALESCE(SUM(LENGTH(r.data)), 0) AS bytes FROM users u"
            " JOIN records r ON r.user_id = u.id AND r.deleted = 0"
            " GROUP BY u.id ORDER BY bytes DESC LIMIT 10"))) {
        while (query.next())
            use.topUsers.append(qMakePair(query.value(0).toString(), query.value(1).toLongLong()));
    }
    return use;
}
