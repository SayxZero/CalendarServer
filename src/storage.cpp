#include "storage.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace {

/*! Written into PRAGMA user_version. */
const int kSchemaVersion = 2;

QString loginKey(const QString &login)
{
    return login.trimmed().toLower();
}

UserInfo readUser(const QSqlQuery &query)
{
    UserInfo user;
    user.id = query.value(QStringLiteral("id")).toLongLong();
    user.login = query.value(QStringLiteral("login")).toString();
    user.displayName = query.value(QStringLiteral("display_name")).toString();
    user.createdAt = QDateTime::fromString(query.value(QStringLiteral("created_at")).toString(),
                                           Qt::ISODate);
    user.revision = query.value(QStringLiteral("revision")).toLongLong();
    return user;
}

SyncRecord readRecord(const QSqlQuery &query)
{
    SyncRecord record;
    record.kind = query.value(QStringLiteral("kind")).toString();
    record.id = query.value(QStringLiteral("id")).toString();
    record.deleted = query.value(QStringLiteral("deleted")).toBool();
    record.modified = query.value(QStringLiteral("modified")).toLongLong();
    record.revision = query.value(QStringLiteral("revision")).toLongLong();
    record.data = query.value(QStringLiteral("data")).toByteArray();
    record.device = query.value(QStringLiteral("device")).toString();
    return record;
}

QString nowText()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

} // namespace

Storage::Storage()
    : m_connectionName(QStringLiteral("server-%1").arg(QUuid::createUuid().toString(QUuid::Id128)))
{
}

Storage::~Storage()
{
    close();
}

bool Storage::open(const QString &path, QString *error)
{
    close();

    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        m_lastError = QStringLiteral("Не удалось создать папку %1").arg(info.absolutePath());
        if (error)
            *error = m_lastError;
        return false;
    }

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        m_lastError = m_db.lastError().text();
        if (error)
            *error = m_lastError;
        return false;
    }

    const QStringList statements = {
        QStringLiteral("PRAGMA journal_mode = WAL"),
        QStringLiteral("PRAGMA foreign_keys = ON"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS users ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "login TEXT NOT NULL,"
                       "login_key TEXT NOT NULL UNIQUE,"
                       "display_name TEXT,"
                       "password_hash TEXT NOT NULL,"
                       "created_at TEXT NOT NULL,"
                       "revision INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS tokens ("
                       "token_hash BLOB PRIMARY KEY,"
                       "user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
                       "device TEXT,"
                       "created_at TEXT NOT NULL,"
                       "last_used_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS records ("
                       "user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
                       "kind TEXT NOT NULL,"
                       "id TEXT NOT NULL,"
                       "data BLOB,"
                       "deleted INTEGER NOT NULL DEFAULT 0,"
                       "modified INTEGER NOT NULL,"
                       "revision INTEGER NOT NULL,"
                       "device TEXT,"
                       "PRIMARY KEY (user_id, kind, id))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_records_revision"
                       " ON records(user_id, revision)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tokens_user ON tokens(user_id)"),
        QStringLiteral("PRAGMA user_version = %1").arg(kSchemaVersion),
    };
    for (const QString &statement : statements) {
        if (!exec(statement)) {
            if (error)
                *error = m_lastError;
            close();
            return false;
        }
    }

    // Version 2 of the schema: the shared calendars. Its tables are created by
    // the store that owns them, on the same connection.
    m_shares = std::make_unique<ShareStore>(m_db);
    if (!m_shares->applySchema(&m_lastError)) {
        if (error)
            *error = m_lastError;
        close();
        return false;
    }
    return true;
}

void Storage::close()
{
    // The share store holds a handle of its own to the same connection; it has
    // to let go before the connection may be removed.
    m_shares.reset();
    if (m_db.isOpen()) {
        QSqlQuery checkpoint(m_db);
        checkpoint.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
        m_db.close();
    }
    if (QSqlDatabase::contains(m_connectionName)) {
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool Storage::exec(const QString &statement)
{
    QSqlQuery query(m_db);
    if (query.exec(statement))
        return true;
    m_lastError = query.lastError().text();
    qWarning().noquote() << "SQL failed:" << statement << m_lastError;
    return false;
}

// --------------------------------------------------------------------- users

UserInfo Storage::userByLogin(const QString &login)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT * FROM users WHERE login_key = :key"));
    query.bindValue(QStringLiteral(":key"), loginKey(login));
    if (query.exec() && query.next())
        return readUser(query);
    return UserInfo();
}

UserInfo Storage::userById(qint64 id)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT * FROM users WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), id);
    if (query.exec() && query.next())
        return readUser(query);
    return UserInfo();
}

QList<UserInfo> Storage::users()
{
    QList<UserInfo> result;
    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("SELECT * FROM users ORDER BY login_key"))) {
        while (query.next())
            result.append(readUser(query));
    }
    return result;
}

UserInfo Storage::createUser(const QString &login, const QString &displayName,
                             const QString &passwordHash)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO users (login, login_key, display_name, password_hash, created_at)"
        " VALUES (:login, :key, :name, :hash, :created)"));
    query.bindValue(QStringLiteral(":login"), login.trimmed());
    query.bindValue(QStringLiteral(":key"), loginKey(login));
    query.bindValue(QStringLiteral(":name"), displayName.trimmed());
    query.bindValue(QStringLiteral(":hash"), passwordHash);
    query.bindValue(QStringLiteral(":created"), nowText());
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return UserInfo();
    }
    return userById(query.lastInsertId().toLongLong());
}

QString Storage::passwordHash(qint64 userId)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT password_hash FROM users WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), userId);
    if (query.exec() && query.next())
        return query.value(0).toString();
    return QString();
}

bool Storage::setPasswordHash(qint64 userId, const QString &hash)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE users SET password_hash = :hash WHERE id = :id"));
    query.bindValue(QStringLiteral(":hash"), hash);
    query.bindValue(QStringLiteral(":id"), userId);
    return query.exec() && query.numRowsAffected() == 1;
}

bool Storage::setDisplayName(qint64 userId, const QString &name)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE users SET display_name = :name WHERE id = :id"));
    query.bindValue(QStringLiteral(":name"), name.trimmed());
    query.bindValue(QStringLiteral(":id"), userId);
    return query.exec() && query.numRowsAffected() == 1;
}

bool Storage::deleteUser(qint64 userId)
{
    if (!m_db.transaction())
        return false;
    for (const QString &table :
         { QStringLiteral("records"), QStringLiteral("tokens"), QStringLiteral("users") }) {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral("DELETE FROM %1 WHERE %2 = :id")
                          .arg(table, table == QLatin1String("users") ? QStringLiteral("id")
                                                                       : QStringLiteral("user_id")));
        query.bindValue(QStringLiteral(":id"), userId);
        if (!query.exec()) {
            m_lastError = query.lastError().text();
            m_db.rollback();
            return false;
        }
    }
    return m_db.commit();
}

// -------------------------------------------------------------------- tokens

bool Storage::addToken(const QByteArray &tokenHash, qint64 userId, const QString &device)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO tokens (token_hash, user_id, device, created_at, last_used_at)"
        " VALUES (:hash, :user, :device, :now, :now)"));
    query.bindValue(QStringLiteral(":hash"), tokenHash);
    query.bindValue(QStringLiteral(":user"), userId);
    query.bindValue(QStringLiteral(":device"), device.left(200));
    query.bindValue(QStringLiteral(":now"), nowText());
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

qint64 Storage::userForToken(const QByteArray &tokenHash, int lifetimeDays)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT user_id, last_used_at FROM tokens WHERE token_hash = :hash"));
    query.bindValue(QStringLiteral(":hash"), tokenHash);
    if (!query.exec() || !query.next())
        return 0;

    const qint64 userId = query.value(0).toLongLong();
    const QDateTime lastUsed = QDateTime::fromString(query.value(1).toString(), Qt::ISODate);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (lastUsed.isValid() && lastUsed.addDays(lifetimeDays) < now) {
        removeToken(tokenHash);
        return 0;
    }

    // Stamping every request would write to disk on each sync; once a minute is enough.
    if (!lastUsed.isValid() || lastUsed.secsTo(now) > 60) {
        QSqlQuery touch(m_db);
        touch.prepare(QStringLiteral("UPDATE tokens SET last_used_at = :now WHERE token_hash = :hash"));
        touch.bindValue(QStringLiteral(":now"), now.toString(Qt::ISODate));
        touch.bindValue(QStringLiteral(":hash"), tokenHash);
        touch.exec();
    }
    return userId;
}

bool Storage::removeToken(const QByteArray &tokenHash)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM tokens WHERE token_hash = :hash"));
    query.bindValue(QStringLiteral(":hash"), tokenHash);
    return query.exec();
}

bool Storage::removeOtherTokens(qint64 userId, const QByteArray &keepHash)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM tokens WHERE user_id = :user AND token_hash <> :keep"));
    query.bindValue(QStringLiteral(":user"), userId);
    query.bindValue(QStringLiteral(":keep"), keepHash);
    return query.exec();
}

int Storage::tokenCount(qint64 userId)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM tokens WHERE user_id = :user"));
    query.bindValue(QStringLiteral(":user"), userId);
    if (query.exec() && query.next())
        return query.value(0).toInt();
    return 0;
}

// ------------------------------------------------------------------- records

SyncRecord Storage::recordAt(qint64 userId, const QString &kind, const QString &id, bool *found)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT * FROM records WHERE user_id = :user AND kind = :kind AND id = :id"));
    query.bindValue(QStringLiteral(":user"), userId);
    query.bindValue(QStringLiteral(":kind"), kind);
    query.bindValue(QStringLiteral(":id"), id);
    if (query.exec() && query.next()) {
        *found = true;
        return readRecord(query);
    }
    *found = false;
    return SyncRecord();
}

SyncOutcome Storage::sync(qint64 userId, qint64 since, const QList<SyncRecord> &incoming,
                          const QString &device)
{
    SyncOutcome outcome;
    const UserInfo user = userById(userId);
    if (!user.isValid()) {
        m_lastError = QStringLiteral("unknown user");
        return outcome;
    }

    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        return outcome;
    }

    qint64 revision = user.revision;
    QSet<QString> accepted;
    QList<SyncRecord> rejected;

    QSqlQuery write(m_db);
    write.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO records (user_id, kind, id, data, deleted, modified, revision,"
        " device) VALUES (:user, :kind, :id, :data, :deleted, :modified, :revision, :device)"));

    // The changes are applied in the order they arrived, which for a shared
    // calendar is the queue the whole account sees: one request, one place in
    // the line, and the next one already sees what the previous one wrote.
    for (const SyncRecord &change : incoming) {
        bool found = false;
        const SyncRecord stored = recordAt(userId, change.kind, change.id, &found);

        // A record of a shared calendar may have to travel further than this
        // record set - or may not be the sender's to change at all. The share
        // is asked before the plain "newest wins" below, because the answers
        // here are questions for the user and that rule is silent: a member who
        // edited while offline would otherwise lose the work without a word,
        // the owner's deletion being the later change of the two.
        const ShareStore::Route route =
            m_shares->routeFor(userId, change.kind, change.data,
                               found ? ShareStore::calendarOf(stored.data) : QString());
        if (route.shared && !route.allowed) {
            ShareConflict conflict;
            conflict.kind = change.kind;
            conflict.id = change.id;
            conflict.calendarId = route.senderCalendarId;
            conflict.reason = QStringLiteral("read_only");
            conflict.data = change.data;
            outcome.conflicts.append(conflict);
            if (found)
                rejected.append(stored);
            continue;
        }

        // The owner holds the original. A member editing a record the owner has
        // already deleted is not overruled silently: the planner asks whether to
        // bring it back or to drop the change.
        if (route.shared && !route.senderIsOwner && !change.deleted && !change.restore) {
            bool ownerHas = false;
            const SyncRecord original =
                recordAt(route.share.ownerId, change.kind, change.id, &ownerHas);
            if (ownerHas && original.deleted) {
                ShareConflict conflict;
                conflict.kind = change.kind;
                conflict.id = change.id;
                conflict.calendarId = route.senderCalendarId;
                conflict.reason = QStringLiteral("deleted");
                conflict.data = change.data;
                outcome.conflicts.append(conflict);
                continue;
            }
        }

        if (found && stored.modified > change.modified) {
            // Another device changed it later: that version stays and goes back.
            rejected.append(stored);
            continue;
        }

        ++revision;
        write.bindValue(QStringLiteral(":user"), userId);
        write.bindValue(QStringLiteral(":kind"), change.kind);
        write.bindValue(QStringLiteral(":id"), change.id);
        write.bindValue(QStringLiteral(":data"), change.deleted ? QByteArray() : change.data);
        write.bindValue(QStringLiteral(":deleted"), change.deleted ? 1 : 0);
        write.bindValue(QStringLiteral(":modified"), change.modified);
        write.bindValue(QStringLiteral(":revision"), revision);
        write.bindValue(QStringLiteral(":device"), device.left(200));
        if (!write.exec()) {
            m_lastError = write.lastError().text();
            m_db.rollback();
            return outcome;
        }
        accepted.insert(change.key());

        if (route.shared) {
            const int written = m_shares->fanOut(route.share, userId, change.kind, change.id,
                                                 change.data, change.deleted, change.modified,
                                                 device);
            if (written < 0) {
                m_lastError = m_shares->lastError();
                m_db.rollback();
                return outcome;
            }
        }
    }

    if (revision != user.revision) {
        QSqlQuery bump(m_db);
        bump.prepare(QStringLiteral("UPDATE users SET revision = :revision WHERE id = :id"));
        bump.bindValue(QStringLiteral(":revision"), revision);
        bump.bindValue(QStringLiteral(":id"), userId);
        if (!bump.exec()) {
            m_lastError = bump.lastError().text();
            m_db.rollback();
            return outcome;
        }
    }

    if (!m_db.commit()) {
        m_lastError = m_db.lastError().text();
        m_db.rollback();
        return outcome;
    }

    // A device that starts from scratch has nothing to delete, so the
    // tombstones of records it never saw are left out of its first download.
    QSqlQuery changes(m_db);
    changes.prepare(QStringLiteral(
        "SELECT * FROM records WHERE user_id = :user AND revision > :since"
        "%1 ORDER BY revision")
                        .arg(since <= 0 ? QStringLiteral(" AND deleted = 0") : QString()));
    changes.bindValue(QStringLiteral(":user"), userId);
    changes.bindValue(QStringLiteral(":since"), qMax<qint64>(0, since));
    if (!changes.exec()) {
        m_lastError = changes.lastError().text();
        return outcome;
    }

    QSet<QString> listed;
    while (changes.next()) {
        const SyncRecord record = readRecord(changes);
        if (accepted.contains(record.key()))
            continue;
        listed.insert(record.key());
        outcome.changes.append(record);
    }
    for (const SyncRecord &record : rejected) {
        if (!listed.contains(record.key()))
            outcome.changes.append(record);
    }

    outcome.revision = revision;
    outcome.accepted = int(accepted.size());
    outcome.rejected = int(rejected.size());
    outcome.ok = true;
    return outcome;
}

QByteArray Storage::recordData(qint64 userId, const QString &kind, const QString &id)
{
    bool found = false;
    const SyncRecord record = recordAt(userId, kind, id, &found);
    return found && !record.deleted ? record.data : QByteArray();
}

int Storage::recordCount(qint64 userId, bool includeDeleted)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM records WHERE user_id = :user%1")
                      .arg(includeDeleted ? QString() : QStringLiteral(" AND deleted = 0")));
    query.bindValue(QStringLiteral(":user"), userId);
    if (query.exec() && query.next())
        return query.value(0).toInt();
    return 0;
}
