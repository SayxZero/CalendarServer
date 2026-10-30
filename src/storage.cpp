#include "storage.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QPair>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include <QThread>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace {

/*! Written into PRAGMA user_version. */
const int kSchemaVersion = 3;

/*!
 * How long a connection waits for a write lock another process holds - the
 * admin panel changing a password, say. Writers of this process wait at the
 * gate instead and never get that far.
 */
const int kBusyTimeoutMs = 10000;

qint64 steadyNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

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

/*! The exchanges of one account on one day, counted since the last flush. */
struct PendingActivity
{
    int syncs = 0;
    qint64 changesIn = 0;
    qint64 changesOut = 0;
    qint64 bytesIn = 0;
    qint64 bytesOut = 0;
    QString lastAt;

    void add(const PendingActivity &other)
    {
        syncs += other.syncs;
        changesIn += other.changesIn;
        changesOut += other.changesOut;
        bytesIn += other.bytesIn;
        bytesOut += other.bytesOut;
        lastAt = qMax(lastAt, other.lastAt);
    }
};

} // namespace

/*! What every Storage of the process on one file shares; see Storage. */
struct BaseShared
{
    QMutex gate;

    std::atomic<qint64> writes { 0 };
    std::atomic<qint64> waitNs { 0 };
    std::atomic<qint64> holdNs { 0 };
    std::atomic<qint64> maxWaitNs { 0 };
    std::atomic<qint64> commits { 0 };

    QMutex pendingLock;
    QHash<QPair<qint64, QString>, PendingActivity> activity;   //!< by account and day
    QHash<QByteArray, QString> touches;                         //!< token hash -> last use

    std::atomic<Storage::Durability> durability { Storage::Durability::Full };

    /*! The commit thread while group commit is on; transact() looks here. */
    std::atomic<Storage::Committer *> committer { nullptr };
    std::unique_ptr<Storage::Committer> committerOwner;
    QMutex committerLock;

    void noteWait(qint64 waited)
    {
        waitNs += waited;
        qint64 longest = maxWaitNs.load();
        while (waited > longest && !maxWaitNs.compare_exchange_weak(longest, waited)) {
        }
    }

    /*! The one for @p path, made on first use and gone with its last Storage. */
    static std::shared_ptr<BaseShared> of(const QString &path)
    {
        static QMutex lock;
        static QHash<QString, std::weak_ptr<BaseShared>> bases;

        QString key = QFileInfo(path).canonicalFilePath();
        if (key.isEmpty())
            key = QFileInfo(path).absoluteFilePath();
#ifdef Q_OS_WIN
        key = key.toLower();
#endif
        QMutexLocker locker(&lock);
        std::shared_ptr<BaseShared> shared = bases.value(key).lock();
        if (!shared) {
            shared = std::make_shared<BaseShared>();
            bases.insert(key, shared);
        }
        return shared;
    }

    ~BaseShared();
};

// ---------------------------------------------------------------------- write

Storage::Write::Write(Storage &storage)
    : m_storage(storage)
{
    if (!storage.m_base || !storage.m_db.isOpen()) {
        storage.m_lastError = QStringLiteral("база не открыта");
        return;
    }
    if (storage.m_writeDepth++ > 0) {
        // Inside a transaction already: this one is a part of it.
        m_ok = !storage.m_writeFailed;
        return;
    }

    m_outer = true;
    storage.m_writeFailed = false;
    BaseShared &base = *storage.m_base;
    const qint64 asked = steadyNs();
    base.gate.lock();
    m_heldSince = steadyNs();
    base.noteWait(m_heldSince - asked);

    // IMMEDIATE takes the write lock at once. A deferred transaction would
    // read first and upgrade later, and an upgrade after somebody else's
    // commit fails with SQLITE_BUSY without waiting at all.
    QSqlQuery begin(storage.m_db);
    m_ok = begin.exec(QStringLiteral("BEGIN IMMEDIATE"));
    if (!m_ok) {
        storage.m_lastError = begin.lastError().text();
        storage.m_writeFailed = true;
    }
}

Storage::Write::~Write()
{
    if (!m_storage.m_base || m_storage.m_writeDepth == 0)
        return;
    if (!m_outer) {
        --m_storage.m_writeDepth;
        return;
    }
    if (!m_done) {
        if (m_ok) {
            QSqlQuery rollback(m_storage.m_db);
            rollback.exec(QStringLiteral("ROLLBACK"));
        }
        m_done = true;
    }
    m_storage.m_writeDepth = 0;
    m_storage.m_writeFailed = false;
    BaseShared &base = *m_storage.m_base;
    base.holdNs += steadyNs() - m_heldSince;
    if (m_counted) {
        ++base.writes;
        if (m_ok)
            ++base.commits;
    }
    base.gate.unlock();
}

bool Storage::Write::commit()
{
    if (!m_outer)
        return m_ok && !m_storage.m_writeFailed;
    if (m_done)
        return false;
    m_done = true;
    if (!m_ok)
        return false;
    QSqlQuery query(m_storage.m_db);
    if (m_storage.m_writeFailed) {
        query.exec(QStringLiteral("ROLLBACK"));
        return false;
    }
    if (!query.exec(QStringLiteral("COMMIT"))) {
        m_storage.m_lastError = query.lastError().text();
        QSqlQuery rollback(m_storage.m_db);
        rollback.exec(QStringLiteral("ROLLBACK"));
        return false;
    }
    return true;
}

void Storage::Write::fail()
{
    m_storage.m_writeFailed = true;
}

// ------------------------------------------------------------- group commit

/*!
 * The commit thread of one base. Operations of transact() from every thread
 * line up here; the thread takes all that wait (up to the batch), runs each
 * under a savepoint of its own in one transaction, and commits them with one
 * fsync. Whoever sent an operation waits until its commit is done, so what
 * transact() reports as written is on the disk exactly as without it.
 */
class Storage::Committer
{
public:
    Committer(const QString &path, int batch)
        : m_path(path)
        , m_batch(qMax(1, batch))
    {
    }

    ~Committer() { stop(); }

    bool start(QString *error)
    {
        m_thread = QThread::create([this] { loop(); });
        m_thread->setObjectName(QStringLiteral("commit"));
        m_thread->start();
        std::unique_lock<std::mutex> lock(m_lock);
        m_wake.wait(lock, [this] { return m_started; });
        if (m_startError.isEmpty())
            return true;
        const QString problem = m_startError;
        lock.unlock();
        stop();
        if (error)
            *error = problem;
        return false;
    }

    /*! Takes no more work, finishes what waits, and ends the thread. */
    void stop()
    {
        if (!m_thread)
            return;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_stopping = true;
        }
        m_wake.notify_all();
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }

    bool onThread() const { return QThread::currentThread() == m_thread; }

    /*! Runs @p work in the next batch and waits for its commit. */
    bool run(const std::function<bool(Storage &)> &work, BaseShared &base, QString *error)
    {
        Job job;
        job.work = &work;
        const qint64 asked = steadyNs();
        {
            std::unique_lock<std::mutex> lock(m_lock);
            if (m_stopping) {
                if (error)
                    *error = QStringLiteral("сервер останавливается");
                return false;
            }
            m_jobs.push_back(&job);
            m_wake.notify_all();
            m_done.wait(lock, [&job] { return job.done; });
        }
        base.noteWait(steadyNs() - asked);
        ++base.writes;
        if (!job.ok && error)
            *error = job.error;
        return job.ok;
    }

private:
    struct Job
    {
        const std::function<bool(Storage &)> *work = nullptr;
        bool done = false;
        bool ok = false;
        QString error;
    };

    void loop()
    {
        Storage storage;
        QString problem;
        const bool opened = storage.open(m_path, &problem, OpenMode::Attach);
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_started = true;
            if (!opened)
                m_startError = problem.isEmpty() ? QStringLiteral("не открылась база") : problem;
        }
        m_wake.notify_all();
        if (!opened)
            return;

        std::vector<Job *> batch;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m_lock);
                m_wake.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
                if (m_jobs.empty())
                    break;   // stopping, and nothing left
                while (!m_jobs.empty() && int(batch.size()) < m_batch) {
                    batch.push_back(m_jobs.front());
                    m_jobs.pop_front();
                }
            }
            commit(storage, batch);
            {
                std::lock_guard<std::mutex> lock(m_lock);
                for (Job *job : batch)
                    job->done = true;
            }
            m_done.notify_all();
            batch.clear();
        }
        storage.close();
    }

    void commit(Storage &storage, const std::vector<Job *> &batch)
    {
        Write write(storage);
        write.m_counted = false;   // the operations count themselves in run()
        if (!write.ok()) {
            for (Job *job : batch)
                job->error = storage.lastError();
            return;
        }
        for (Job *job : batch) {
            // Each operation under a savepoint: a failure takes back only its
            // own writes, the rest of the batch goes on.
            storage.m_writeFailed = false;
            storage.m_lastError.clear();
            QSqlQuery(storage.m_db).exec(QStringLiteral("SAVEPOINT operation"));
            bool ok = false;
            try {
                ok = (*job->work)(storage) && !storage.m_writeFailed;
            } catch (const std::exception &failure) {
                storage.m_lastError = QString::fromLocal8Bit(failure.what());
            } catch (...) {
                storage.m_lastError = QStringLiteral("операция прервана");
            }
            QSqlQuery savepoint(storage.m_db);
            if (ok) {
                savepoint.exec(QStringLiteral("RELEASE operation"));
            } else {
                savepoint.exec(QStringLiteral("ROLLBACK TO operation"));
                savepoint.exec(QStringLiteral("RELEASE operation"));
                job->error = storage.m_lastError.isEmpty() ? QStringLiteral("запись не удалась")
                                                           : storage.m_lastError;
            }
            job->ok = ok;
        }
        storage.m_writeFailed = false;
        if (!write.commit()) {
            for (Job *job : batch) {
                if (job->ok) {
                    job->ok = false;
                    job->error = storage.lastError();
                }
            }
            return;
        }
        ++storage.m_base->commits;
    }

    const QString m_path;
    const int m_batch;
    QThread *m_thread = nullptr;
    std::mutex m_lock;
    std::condition_variable m_wake;
    std::condition_variable m_done;
    std::deque<Job *> m_jobs;
    bool m_stopping = false;
    bool m_started = false;
    QString m_startError;
};

BaseShared::~BaseShared() = default;

bool Storage::transact(const std::function<bool(Storage &)> &work)
{
    if (!m_base || !m_db.isOpen()) {
        m_lastError = QStringLiteral("база не открыта");
        return false;
    }
    // Inside a transaction of this connection already: a part of it.
    if (m_writeDepth > 0) {
        if (work(*this) && !m_writeFailed)
            return true;
        m_writeFailed = true;
        return false;
    }
    Committer *committer = m_base->committer.load();
    if (committer && !committer->onThread()) {
        QString error;
        if (committer->run(work, *m_base, &error))
            return true;
        m_lastError = error;
        return false;
    }
    Write write(*this);
    if (!write.ok())
        return false;
    if (!work(*this)) {
        write.fail();
        return false;
    }
    return write.commit();
}

bool Storage::enableGroupCommit(int batch, QString *error)
{
    if (!m_base || !m_db.isOpen()) {
        if (error)
            *error = QStringLiteral("база не открыта");
        return false;
    }
    QMutexLocker locker(&m_base->committerLock);
    if (m_base->committerOwner)
        return true;
    auto committer = std::make_unique<Committer>(m_db.databaseName(), batch);
    if (!committer->start(error))
        return false;
    m_base->committer = committer.get();
    m_base->committerOwner = std::move(committer);
    m_ownsCommitter = true;
    return true;
}

void Storage::disableGroupCommit()
{
    if (!m_base)
        return;
    QMutexLocker locker(&m_base->committerLock);
    // Nobody new finds it, then what waits is committed and the thread ends.
    m_base->committer = nullptr;
    m_base->committerOwner.reset();
    m_ownsCommitter = false;
}

int Storage::synchronousMode()
{
    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("PRAGMA synchronous")) && query.next())
        return query.value(0).toInt();
    return -1;
}

void Storage::setDurability(Durability durability)
{
    if (!m_base)
        return;
    m_base->durability = durability;
    exec(durability == Durability::Full ? QStringLiteral("PRAGMA synchronous = FULL")
                                        : QStringLiteral("PRAGMA synchronous = NORMAL"));
}


// ---------------------------------------------------------------------- base

Storage::Storage()
    : m_connectionName(QStringLiteral("server-%1").arg(QUuid::createUuid().toString(QUuid::Id128)))
{
}

Storage::~Storage()
{
    close();
}

bool Storage::open(const QString &path, QString *error, OpenMode mode)
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
    m_db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=%1").arg(kBusyTimeoutMs));
    if (!m_db.open()) {
        m_lastError = m_db.lastError().text();
        if (error)
            *error = m_lastError;
        return false;
    }
    m_base = BaseShared::of(path);
    // What a commit waits for is a setting of the connection: every one of
    // the process gets the one the server chose.
    if (m_base->durability.load() == Durability::Normal)
        exec(QStringLiteral("PRAGMA synchronous = NORMAL"));

    if (mode == OpenMode::Attach) {
        // The first connection has made the schema; this one only needs the
        // settings that live with a connection rather than with the file.
        if (!exec(QStringLiteral("PRAGMA foreign_keys = ON"))) {
            if (error)
                *error = m_lastError;
            close();
            return false;
        }
        m_shares = std::make_unique<ShareStore>(m_db);
        return true;
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
    if (m_db.isOpen() && m_base) {
        flushPending();
        // The commit thread holds a connection of its own to the base: it
        // goes with the Storage that started it, or it would outlive them all.
        if (m_ownsCommitter)
            disableGroupCommit();
        // The last connection of the process folds the WAL back into the
        // file; one that closes next to others working would only wait for
        // them in the busy handler.
        if (m_base.use_count() == 1) {
            QSqlQuery checkpoint(m_db);
            checkpoint.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
        }
    }
    m_shares.reset();
    if (m_db.isOpen())
        m_db.close();
    m_base.reset();
    m_writeDepth = 0;
    m_writeFailed = false;
    if (QSqlDatabase::contains(m_connectionName)) {
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool Storage::flushPending()
{
    if (!m_base || !m_db.isOpen())
        return false;

    QHash<QPair<qint64, QString>, PendingActivity> activity;
    QHash<QByteArray, QString> touches;
    {
        QMutexLocker locker(&m_base->pendingLock);
        activity.swap(m_base->activity);
        touches.swap(m_base->touches);
    }
    if (activity.isEmpty() && touches.isEmpty())
        return true;

    const bool done = transact([&](Storage &storage) {
        // An account deleted since its exchange is skipped rather than
        // failing the rest on its foreign key.
        QSqlQuery count(storage.m_db);
        count.prepare(QStringLiteral(
            "INSERT INTO activity (user_id, day, syncs, changes_in, changes_out, bytes_in,"
            " bytes_out, last_at)"
            " SELECT :user, :day, :syncs, :cin, :cout, :bin, :bout, :at"
            " WHERE EXISTS (SELECT 1 FROM users WHERE id = :user2)"
            " ON CONFLICT(user_id, day) DO UPDATE SET syncs = syncs + excluded.syncs,"
            " changes_in = changes_in + excluded.changes_in,"
            " changes_out = changes_out + excluded.changes_out,"
            " bytes_in = bytes_in + excluded.bytes_in,"
            " bytes_out = bytes_out + excluded.bytes_out,"
            " last_at = MAX(last_at, excluded.last_at)"));
        for (auto it = activity.constBegin(); it != activity.constEnd(); ++it) {
            count.bindValue(QStringLiteral(":user"), it.key().first);
            count.bindValue(QStringLiteral(":user2"), it.key().first);
            count.bindValue(QStringLiteral(":day"), it.key().second);
            count.bindValue(QStringLiteral(":syncs"), it->syncs);
            count.bindValue(QStringLiteral(":cin"), it->changesIn);
            count.bindValue(QStringLiteral(":cout"), it->changesOut);
            count.bindValue(QStringLiteral(":bin"), it->bytesIn);
            count.bindValue(QStringLiteral(":bout"), it->bytesOut);
            count.bindValue(QStringLiteral(":at"), it->lastAt);
            if (!count.exec()) {
                storage.m_lastError = count.lastError().text();
                return false;
            }
        }
        QSqlQuery touch(storage.m_db);
        touch.prepare(QStringLiteral("UPDATE tokens SET last_used_at = :now WHERE token_hash = :hash"));
        for (auto it = touches.constBegin(); it != touches.constEnd(); ++it) {
            touch.bindValue(QStringLiteral(":now"), it.value());
            touch.bindValue(QStringLiteral(":hash"), it.key());
            if (!touch.exec()) {
                storage.m_lastError = touch.lastError().text();
                return false;
            }
        }
        return true;
    });
    if (done)
        return true;

    // Kept for the next try rather than lost: the base may only be busy.
    qWarning().noquote() << "Не удалось записать счёт обменов:" << m_lastError;
    QMutexLocker locker(&m_base->pendingLock);
    for (auto it = activity.constBegin(); it != activity.constEnd(); ++it)
        m_base->activity[it.key()].add(it.value());
    for (auto it = touches.constBegin(); it != touches.constEnd(); ++it) {
        if (!m_base->touches.contains(it.key()))
            m_base->touches.insert(it.key(), it.value());
    }
    return false;
}

GateStats Storage::takeGateStats()
{
    GateStats stats;
    if (!m_base)
        return stats;
    stats.writes = m_base->writes.load();
    stats.waitNs = m_base->waitNs.load();
    stats.holdNs = m_base->holdNs.load();
    stats.maxWaitNs = m_base->maxWaitNs.exchange(0);
    stats.commits = m_base->commits.load();
    return stats;
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
    qint64 id = 0;
    const bool ok = transact([&](Storage &storage) {
        QSqlQuery query(storage.m_db);
        query.prepare(QStringLiteral(
            "INSERT INTO users (login, login_key, display_name, password_hash, created_at)"
            " VALUES (:login, :key, :name, :hash, :created)"));
        query.bindValue(QStringLiteral(":login"), login.trimmed());
        query.bindValue(QStringLiteral(":key"), loginKey(login));
        query.bindValue(QStringLiteral(":name"), displayName.trimmed());
        query.bindValue(QStringLiteral(":hash"), passwordHash);
        query.bindValue(QStringLiteral(":created"), nowText());
        if (!query.exec()) {
            storage.m_lastError = query.lastError().text();
            return false;
        }
        id = query.lastInsertId().toLongLong();
        return true;
    });
    // Inside a transaction this is the connection that wrote it; otherwise
    // the user is committed by now and any connection sees it.
    return ok ? userById(id) : UserInfo();
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
    return transact([&](Storage &storage) {
        QSqlQuery query(storage.m_db);
        query.prepare(QStringLiteral("UPDATE users SET password_hash = :hash WHERE id = :id"));
        query.bindValue(QStringLiteral(":hash"), hash);
        query.bindValue(QStringLiteral(":id"), userId);
        if (!query.exec()) {
            storage.m_lastError = query.lastError().text();
            return false;
        }
        return query.numRowsAffected() == 1;
    });
}

bool Storage::setDisplayName(qint64 userId, const QString &name)
{
    return transact([&](Storage &storage) {
        QSqlQuery query(storage.m_db);
        query.prepare(QStringLiteral("UPDATE users SET display_name = :name WHERE id = :id"));
        query.bindValue(QStringLiteral(":name"), name.trimmed());
        query.bindValue(QStringLiteral(":id"), userId);
        if (!query.exec()) {
            storage.m_lastError = query.lastError().text();
            return false;
        }
        return query.numRowsAffected() == 1;
    });
}

bool Storage::deleteUser(qint64 userId)
{
    return transact([&](Storage &storage) {
        // First the shared calendars: the members' copies of this account's
        // calendars go with tombstones, before the account itself is gone.
        if (!storage.m_shares->forgetUser(userId)) {
            storage.m_lastError = storage.m_shares->lastError();
            return false;
        }
        for (const QString &table :
             { QStringLiteral("records"), QStringLiteral("tokens"), QStringLiteral("activity"),
               QStringLiteral("users") }) {
            QSqlQuery query(storage.m_db);
            query.prepare(QStringLiteral("DELETE FROM %1 WHERE %2 = :id")
                              .arg(table, table == QLatin1String("users") ? QStringLiteral("id")
                                                                           : QStringLiteral("user_id")));
            query.bindValue(QStringLiteral(":id"), userId);
            if (!query.exec()) {
                storage.m_lastError = query.lastError().text();
                return false;
            }
        }
        return true;
    });
}

// -------------------------------------------------------------------- tokens

bool Storage::addToken(const QByteArray &tokenHash, qint64 userId, const QString &device)
{
    return transact([&](Storage &storage) {
        QSqlQuery query(storage.m_db);
        query.prepare(QStringLiteral(
            "INSERT INTO tokens (token_hash, user_id, device, created_at, last_used_at)"
            " VALUES (:hash, :user, :device, :now, :now)"));
        query.bindValue(QStringLiteral(":hash"), tokenHash);
        query.bindValue(QStringLiteral(":user"), userId);
        query.bindValue(QStringLiteral(":device"), device.left(200));
        query.bindValue(QStringLiteral(":now"), nowText());
        if (!query.exec()) {
            storage.m_lastError = query.lastError().text();
            return false;
        }
        return true;
    });
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
    query.finish();
    if (lastUsed.isValid() && lastUsed.addDays(lifetimeDays) < now) {
        removeToken(tokenHash);
        return 0;
    }

    // Stamping every request would write to disk on each sync: the stamp is
    // kept in memory and written with the activity (flushPending()), and only
    // when the stored one is a minute old.
    if (!lastUsed.isValid() || lastUsed.secsTo(now) > 60) {
        QMutexLocker locker(&m_base->pendingLock);
        m_base->touches.insert(tokenHash, now.toString(Qt::ISODate));
    }
    return userId;
}

bool Storage::removeToken(const QByteArray &tokenHash)
{
    return transact([&](Storage &storage) {
        QSqlQuery query(storage.m_db);
        query.prepare(QStringLiteral("DELETE FROM tokens WHERE token_hash = :hash"));
        query.bindValue(QStringLiteral(":hash"), tokenHash);
        if (!query.exec()) {
            storage.m_lastError = query.lastError().text();
            return false;
        }
        return true;
    });
}

bool Storage::removeOtherTokens(qint64 userId, const QByteArray &keepHash)
{
    return transact([&](Storage &storage) {
        QSqlQuery query(storage.m_db);
        query.prepare(
            QStringLiteral("DELETE FROM tokens WHERE user_id = :user AND token_hash <> :keep"));
        query.bindValue(QStringLiteral(":user"), userId);
        query.bindValue(QStringLiteral(":keep"), keepHash);
        if (!query.exec()) {
            storage.m_lastError = query.lastError().text();
            return false;
        }
        return true;
    });
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

QList<SessionInfo> Storage::sessions(qint64 userId, int lifetimeDays)
{
    QList<SessionInfo> found;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT token_hash, device, created_at, last_used_at FROM tokens"
                                 " WHERE user_id = :user ORDER BY last_used_at DESC"));
    query.bindValue(QStringLiteral(":user"), userId);
    if (!query.exec())
        return found;

    const QDateTime now = QDateTime::currentDateTimeUtc();
    while (query.next()) {
        SessionInfo session;
        session.tokenHash = query.value(0).toByteArray();
        session.device = query.value(1).toString();
        session.createdAt = QDateTime::fromString(query.value(2).toString(), Qt::ISODate);
        session.lastUsedAt = QDateTime::fromString(query.value(3).toString(), Qt::ISODate);
        // Gone in all but the row: the next use would drop it (userForToken()).
        if (session.lastUsedAt.isValid() && session.lastUsedAt.addDays(lifetimeDays) < now)
            continue;
        found.append(session);
    }
    return found;
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

    // A device that only asks what is new - the most common exchange by far -
    // writes nothing and does not stand in the line at the gate: it reads
    // the cursor, then what is below it. Every record below the cursor is
    // committed by then, because the cursor moves in the same transaction.
    if (incoming.isEmpty()) {
        const UserInfo user = userById(userId);
        if (!user.isValid()) {
            m_lastError = QStringLiteral("unknown user");
            return outcome;
        }
        QSet<QString> listed;
        if (!readChanges(userId, since, user.revision, QSet<QString>(), &outcome.changes, &listed))
            return outcome;
        outcome.revision = user.revision;
        outcome.ok = true;
        return outcome;
    }

    QSet<QString> accepted;
    QList<SyncRecord> rejected;
    qint64 revision = 0;
    const bool written = transact([&](Storage &storage) {
        return storage.applyChanges(userId, incoming, device, &revision, &accepted, &rejected,
                                    &outcome.conflicts);
    });
    if (!written) {
        outcome.conflicts.clear();
        return outcome;
    }

    // The answer is read after the commit, outside the transaction: an
    // exchange that brings a whole planner down to a new device must not hold
    // up the writes of everybody else while it is read.
    QSet<QString> listed;
    if (!readChanges(userId, since, revision, accepted, &outcome.changes, &listed))
        return outcome;
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

bool Storage::applyChanges(qint64 userId, const QList<SyncRecord> &incoming,
                           const QString &device, qint64 *revision, QSet<QString> *accepted,
                           QList<SyncRecord> *rejected, QList<ShareConflict> *conflicts)
{
    // The cursor is read inside the transaction: read before it, two
    // exchanges of one account on two threads would hand out the same
    // revisions, and a device would skip one of the changes for good.
    const UserInfo user = userById(userId);
    if (!user.isValid()) {
        m_lastError = QStringLiteral("unknown user");
        return false;
    }
    qint64 next = user.revision;

    QSqlQuery store(m_db);
    store.prepare(QStringLiteral(
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
            conflicts->append(conflict);
            if (found)
                rejected->append(stored);
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
                conflicts->append(conflict);
                continue;
            }
        }

        if (found && stored.modified > change.modified) {
            // Another device changed it later: that version stays and goes back.
            rejected->append(stored);
            continue;
        }

        ++next;
        store.bindValue(QStringLiteral(":user"), userId);
        store.bindValue(QStringLiteral(":kind"), change.kind);
        store.bindValue(QStringLiteral(":id"), change.id);
        store.bindValue(QStringLiteral(":data"), change.deleted ? QByteArray() : change.data);
        store.bindValue(QStringLiteral(":deleted"), change.deleted ? 1 : 0);
        store.bindValue(QStringLiteral(":modified"), change.modified);
        store.bindValue(QStringLiteral(":revision"), next);
        store.bindValue(QStringLiteral(":device"), device.left(200));
        if (!store.exec()) {
            m_lastError = store.lastError().text();
            return false;
        }
        accepted->insert(change.key());

        if (route.shared) {
            const int written = m_shares->fanOut(route.share, userId, change.kind, change.id,
                                                 change.data, change.deleted, change.modified,
                                                 device);
            if (written < 0) {
                m_lastError = m_shares->lastError();
                return false;
            }
        }
    }

    if (next != user.revision) {
        QSqlQuery bump(m_db);
        bump.prepare(QStringLiteral("UPDATE users SET revision = :revision WHERE id = :id"));
        bump.bindValue(QStringLiteral(":revision"), next);
        bump.bindValue(QStringLiteral(":id"), userId);
        if (!bump.exec()) {
            m_lastError = bump.lastError().text();
            return false;
        }
    }

    *revision = next;
    return true;
}

bool Storage::readChanges(qint64 userId, qint64 since, qint64 upTo, const QSet<QString> &skip,
                          QList<SyncRecord> *changes, QSet<QString> *listed)
{
    // A device that starts from scratch has nothing to delete, so the
    // tombstones of records it never saw are left out of its first download.
    // Revisions above the cursor - another exchange of the account committed
    // meanwhile - wait for the next exchange, which asks from this cursor.
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT * FROM records WHERE user_id = :user AND revision > :since AND revision <= :upto"
        "%1 ORDER BY revision")
                      .arg(since <= 0 ? QStringLiteral(" AND deleted = 0") : QString()));
    query.bindValue(QStringLiteral(":user"), userId);
    query.bindValue(QStringLiteral(":since"), qMax<qint64>(0, since));
    query.bindValue(QStringLiteral(":upto"), upTo);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    while (query.next()) {
        SyncRecord record = readRecord(query);
        if (skip.contains(record.key()))
            continue;
        listed->insert(record.key());
        changes->append(std::move(record));
    }
    return true;
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

void Storage::noteSync(qint64 userId, const SyncTraffic &traffic)
{
    if (!m_base)
        return;
    PendingActivity one;
    one.syncs = 1;
    one.changesIn = traffic.changesIn;
    one.changesOut = traffic.changesOut;
    one.bytesIn = traffic.bytesIn;
    one.bytesOut = traffic.bytesOut;
    one.lastAt = nowText();
    QMutexLocker locker(&m_base->pendingLock);
    m_base->activity[qMakePair(userId, QDate::currentDate().toString(Qt::ISODate))].add(one);
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
