#include "storage.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace {

/*! Written into PRAGMA user_version. */
const int kSchemaVersion = 3;

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

    // An older server must not write its version over a newer base: it would
    // not know what the newer one keeps, and the downgrade would stick.
    int previousVersion = 0;
    {
        QSqlQuery version(m_db);
        if (version.exec(QStringLiteral("PRAGMA user_version")) && version.next())
            previousVersion = version.value(0).toInt();
    }
    if (previousVersion > kSchemaVersion) {
        m_lastError = QStringLiteral("база создана более новой версией сервера (схема %1, "
                                     "эта версия знает до %2)")
                          .arg(previousVersion)
                          .arg(kSchemaVersion);
        if (error)
            *error = m_lastError;
        close();
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
        // Version 3: the exchanges of every account, one row a day, for the
        // statistics of the admin panel. The day is the local one of the server.
        QStringLiteral("CREATE TABLE IF NOT EXISTS activity ("
                       "user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
                       "day TEXT NOT NULL,"
                       "syncs INTEGER NOT NULL DEFAULT 0,"
                       "changes_in INTEGER NOT NULL DEFAULT 0,"
                       "changes_out INTEGER NOT NULL DEFAULT 0,"
                       "bytes_in INTEGER NOT NULL DEFAULT 0,"
                       "bytes_out INTEGER NOT NULL DEFAULT 0,"
                       "last_at TEXT NOT NULL,"
                       "PRIMARY KEY (user_id, day))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_activity_day ON activity(day)"),
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

    // A base from before version 3 has no exchanges counted. What it does know
    // is the day each device was last seen, and that is where the history of
    // the admin panel starts - without the number of syncs of those days.
    if (previousVersion > 0 && previousVersion < 3) {
        QSqlQuery seen(m_db);
        QSqlQuery seed(m_db);
        seed.prepare(QStringLiteral(
            "INSERT INTO activity (user_id, day, last_at) VALUES (:user, :day, :at)"
            " ON CONFLICT(user_id, day) DO UPDATE SET last_at = MAX(last_at, excluded.last_at)"));
        if (seen.exec(QStringLiteral("SELECT user_id, last_used_at FROM tokens"))) {
            while (seen.next()) {
                const QDateTime at = QDateTime::fromString(seen.value(1).toString(), Qt::ISODate);
                if (!at.isValid())
                    continue;
                seed.bindValue(QStringLiteral(":user"), seen.value(0));
                seed.bindValue(QStringLiteral(":day"),
                               at.toLocalTime().date().toString(Qt::ISODate));
                seed.bindValue(QStringLiteral(":at"), at.toUTC().toString(Qt::ISODate));
                seed.exec();
            }
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
         { QStringLiteral("records"), QStringLiteral("tokens"), QStringLiteral("activity"),
           QStringLiteral("users") }) {
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

bool Storage::noteSync(qint64 userId, const SyncTraffic &traffic)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO activity (user_id, day, syncs, changes_in, changes_out, bytes_in, bytes_out,"
        " last_at) VALUES (:user, :day, 1, :cin, :cout, :bin, :bout, :at)"
        " ON CONFLICT(user_id, day) DO UPDATE SET syncs = syncs + 1,"
        " changes_in = changes_in + excluded.changes_in,"
        " changes_out = changes_out + excluded.changes_out,"
        " bytes_in = bytes_in + excluded.bytes_in,"
        " bytes_out = bytes_out + excluded.bytes_out,"
        " last_at = excluded.last_at"));
    query.bindValue(QStringLiteral(":user"), userId);
    query.bindValue(QStringLiteral(":day"), QDate::currentDate().toString(Qt::ISODate));
    query.bindValue(QStringLiteral(":cin"), traffic.changesIn);
    query.bindValue(QStringLiteral(":cout"), traffic.changesOut);
    query.bindValue(QStringLiteral(":bin"), traffic.bytesIn);
    query.bindValue(QStringLiteral(":bout"), traffic.bytesOut);
    query.bindValue(QStringLiteral(":at"), nowText());
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

// --------------------------------------------------------------- whole files

int Storage::schemaVersion()
{
    return kSchemaVersion;
}

namespace {

/*!
 * A connection of its own, read-only, for the static calls below: they run
 * next to an open Storage, and in the admin panel next to the server itself.
 */
class ReadOnlyBase
{
public:
    explicit ReadOnlyBase(const QString &path)
        : m_name(QStringLiteral("readonly-%1").arg(QUuid::createUuid().toString(QUuid::Id128)))
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open())
            m_error = db.lastError().text();
    }

    ~ReadOnlyBase()
    {
        {
            QSqlDatabase db = QSqlDatabase::database(m_name, false);
            if (db.isOpen())
                db.close();
        }
        QSqlDatabase::removeDatabase(m_name);
    }

    bool isOpen() const { return m_error.isEmpty(); }
    QString error() const { return m_error; }
    QSqlDatabase db() const { return QSqlDatabase::database(m_name, false); }

private:
    QString m_name;
    QString m_error;
};

} // namespace

DatabaseInfo Storage::inspect(const QString &path, bool check)
{
    DatabaseInfo info;
    if (!QFileInfo(path).isFile()) {
        info.error = QStringLiteral("файла нет");
        return info;
    }
    ReadOnlyBase base(path);
    if (!base.isOpen()) {
        info.error = base.error();
        return info;
    }

    QSqlQuery query(base.db());
    if (!query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type = 'table'"))) {
        info.error = QStringLiteral("это не база SQLite (%1)").arg(query.lastError().text());
        return info;
    }
    QSet<QString> tables;
    while (query.next())
        tables.insert(query.value(0).toString());
    for (const char *needed : { "users", "tokens", "records" }) {
        if (!tables.contains(QLatin1String(needed))) {
            info.error = QStringLiteral("это не база сервера синхронизации");
            return info;
        }
    }

    if (query.exec(QStringLiteral("PRAGMA user_version")) && query.next())
        info.schemaVersion = query.value(0).toInt();
    if (info.schemaVersion > kSchemaVersion) {
        info.error = QStringLiteral("база создана более новой версией сервера (схема %1)")
                         .arg(info.schemaVersion);
        return info;
    }

    if (check) {
        if (!query.exec(QStringLiteral("PRAGMA quick_check")) || !query.next()) {
            info.error = QStringLiteral("файл не читается целиком: %1").arg(query.lastError().text());
            return info;
        }
        const QString verdict = query.value(0).toString();
        if (verdict != QLatin1String("ok")) {
            info.error = QStringLiteral("файл повреждён: %1").arg(verdict);
            return info;
        }
    }

    if (query.exec(QStringLiteral("SELECT COUNT(*) FROM users")) && query.next())
        info.users = query.value(0).toInt();
    if (query.exec(QStringLiteral("SELECT COUNT(*) FROM records WHERE deleted = 0")) && query.next())
        info.records = query.value(0).toInt();

    QString newest;
    if (query.exec(QStringLiteral("SELECT MAX(last_used_at) FROM tokens")) && query.next())
        newest = query.value(0).toString();
    if (tables.contains(QStringLiteral("activity"))
        && query.exec(QStringLiteral("SELECT MAX(last_at) FROM activity")) && query.next()) {
        newest = qMax(newest, query.value(0).toString());
    }
    info.lastActivity = QDateTime::fromString(newest, Qt::ISODate);
    info.ok = true;
    return info;
}

bool Storage::copyDatabase(const QString &from, const QString &to, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (QFileInfo::exists(to))
        return fail(QStringLiteral("файл уже есть: %1").arg(QDir::toNativeSeparators(to)));
    if (!QDir().mkpath(QFileInfo(to).absolutePath()))
        return fail(QStringLiteral("не удалось создать папку %1")
                        .arg(QDir::toNativeSeparators(QFileInfo(to).absolutePath())));

    QString problem;
    {
        ReadOnlyBase base(from);
        if (!base.isOpen())
            return fail(base.error());
        QString target = QFileInfo(to).absoluteFilePath();
        target.replace(QLatin1Char('\''), QStringLiteral("''"));
        QSqlQuery vacuum(base.db());
        if (!vacuum.exec(QStringLiteral("VACUUM INTO '%1'").arg(target)))
            problem = vacuum.lastError().text();
    }
    if (!problem.isEmpty()) {
        QFile::remove(to);
        return fail(problem);
    }

    // The copy carries the WAL mode of its source in its header; a copy that
    // is meant to be carried around had better be a single file.
    const QString name = QStringLiteral("copy-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(to);
        if (db.open()) {
            QSqlQuery mode(db);
            mode.exec(QStringLiteral("PRAGMA journal_mode = DELETE"));
            mode.finish();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return true;
}

bool Storage::removeDatabase(const QString &path, QString *error)
{
    for (const char *suffix : { "", "-wal", "-shm", "-journal" }) {
        const QString file = path + QLatin1String(suffix);
        if (QFileInfo::exists(file) && !QFile::remove(file)) {
            if (error)
                *error = QStringLiteral("не удалось удалить %1").arg(QDir::toNativeSeparators(file));
            return false;
        }
    }
    return true;
}

Storage::ImportResult Storage::importDatabase(const QString &source, const QString &target,
                                              QString *message)
{
    const auto native = [](const QString &path) { return QDir::toNativeSeparators(path); };
    const auto say = [message](const QString &text) {
        if (message)
            *message = text;
    };

    const QFileInfo from(source);
    if (!from.exists()) {
        say(QStringLiteral("Перенос базы: файла %1 нет, переносить нечего").arg(native(source)));
        return ImportResult::Skipped;
    }
    if (QFileInfo::exists(target)) {
        if (from.canonicalFilePath() == QFileInfo(target).canonicalFilePath()) {
            say(QStringLiteral("Перенос базы: сервер и так работает с %1").arg(native(source)));
            return ImportResult::Skipped;
        }
        // A base that does not open is not replaced: it may be the one with
        // the accounts, and only a person can tell.
        const DatabaseInfo current = inspect(target);
        if (!current.ok) {
            say(QStringLiteral("Перенос базы отменён: %1 не открывается: %2")
                    .arg(native(target), current.error));
            return ImportResult::Failed;
        }
        if (current.users > 0) {
            say(QStringLiteral("Перенос базы не нужен: в %1 уже есть аккаунты (%2)")
                    .arg(native(target))
                    .arg(current.users));
            return ImportResult::Skipped;
        }
    }

    const DatabaseInfo old = inspect(source);
    if (!old.ok) {
        say(QStringLiteral("Перенос базы отменён: %1 не открывается: %2").arg(native(source), old.error));
        return ImportResult::Failed;
    }
    if (old.users == 0) {
        say(QStringLiteral("Перенос базы: в %1 нет аккаунтов, переносить нечего").arg(native(source)));
        return ImportResult::Skipped;
    }

    QString problem;
    if (!removeDatabase(target, &problem) || !copyDatabase(source, target, &problem)) {
        say(QStringLiteral("Перенос базы не удался: %1").arg(problem));
        return ImportResult::Failed;
    }
    // The copy is brought up to the schema of this version at once, and so it
    // is also checked to open the way the server is going to open it.
    Storage storage;
    if (!storage.open(target, &problem)) {
        say(QStringLiteral("Перенесённая база не открывается: %1").arg(problem));
        return ImportResult::Failed;
    }
    storage.close();
    say(QStringLiteral("Перенесена база %1 в %2: аккаунтов %3, записей %4")
            .arg(native(source), native(target))
            .arg(old.users)
            .arg(old.records));
    return ImportResult::Imported;
}
