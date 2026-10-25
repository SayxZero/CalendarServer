#include "backups.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPair>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace {

QString native(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

/*! A connection for one job, closed and removed when it goes. */
class Connection
{
public:
    Connection(const QString &path, bool readOnly)
        : m_name(QStringLiteral("backup-%1").arg(QUuid::createUuid().toString(QUuid::Id128)))
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        db.setDatabaseName(path);
        if (readOnly)
            db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open())
            m_error = db.lastError().text();
    }

    ~Connection()
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

/*! Login key and revision of every account, by id. */
QHash<qint64, QPair<QString, qint64>> revisionsOf(const QString &path)
{
    QHash<qint64, QPair<QString, qint64>> result;
    if (!QFileInfo::exists(path))
        return result;
    Connection base(path, true);
    if (!base.isOpen())
        return result;
    QSqlQuery query(base.db());
    if (query.exec(QStringLiteral("SELECT id, login_key, revision FROM users"))) {
        while (query.next()) {
            result.insert(query.value(0).toLongLong(),
                          qMakePair(query.value(1).toString(), query.value(2).toLongLong()));
        }
    }
    return result;
}

/*! The files of a base copied as they are, for a base too broken to be read. */
bool copyRaw(const QString &from, const QString &to)
{
    if (!QFile::copy(from, to))
        return false;
    for (const char *suffix : { "-wal", "-journal" }) {
        const QString side = from + QLatin1String(suffix);
        if (QFileInfo::exists(side))
            QFile::copy(side, to + QLatin1String(suffix));
    }
    return true;
}

} // namespace

QString Backups::fileName(const QDateTime &moment, const QString &prefix)
{
    return QStringLiteral("%1-%2.db").arg(prefix, moment.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")));
}

QList<BackupFile> Backups::list(const QString &folder)
{
    QList<BackupFile> result;
    const QFileInfoList files =
        QDir(folder).entryInfoList({ QStringLiteral("*.db") }, QDir::Files, QDir::Time);
    for (const QFileInfo &file : files) {
        BackupFile backup;
        backup.path = file.absoluteFilePath();
        backup.modified = file.lastModified();
        backup.size = file.size();
        backup.info = Storage::inspect(backup.path);
        result.append(backup);
    }
    return result;
}

bool Backups::create(const QString &database, const QString &folder, QString *created,
                     QString *error)
{
    QString path = QDir(folder).absoluteFilePath(fileName(QDateTime::currentDateTime()));
    // Two copies within one second: the second gets a number.
    for (int n = 2; QFileInfo::exists(path); ++n) {
        path = QDir(folder).absoluteFilePath(
            fileName(QDateTime::currentDateTime(), QStringLiteral("server")).chopped(3)
            + QStringLiteral("-%1.db").arg(n));
    }
    if (!Storage::copyDatabase(database, path, error))
        return false;
    if (created)
        *created = path;
    return true;
}

RestoreOutcome Backups::restore(const QString &backup, const QString &database,
                                const QString &safetyFolder)
{
    RestoreOutcome outcome;
    const DatabaseInfo info = Storage::inspect(backup, true);
    if (!info.ok) {
        outcome.error = QStringLiteral("Копия %1 не годится: %2").arg(native(backup), info.error);
        return outcome;
    }
    if (QFileInfo(backup).canonicalFilePath() == QFileInfo(database).canonicalFilePath()) {
        outcome.error = QStringLiteral("Это и есть рабочая база сервера.");
        return outcome;
    }

    const QHash<qint64, QPair<QString, qint64>> revisions = revisionsOf(database);

    // The base as it was, kept aside before anything is deleted.
    if (QFileInfo::exists(database)) {
        if (!QDir().mkpath(safetyFolder)) {
            outcome.error = QStringLiteral("Не удалось создать папку %1").arg(native(safetyFolder));
            return outcome;
        }
        outcome.safetyCopy = QDir(safetyFolder)
                                 .absoluteFilePath(fileName(QDateTime::currentDateTime(),
                                                            QStringLiteral("before-restore")));
        QString problem;
        if (!Storage::copyDatabase(database, outcome.safetyCopy, &problem)
            && !copyRaw(database, outcome.safetyCopy)) {
            outcome.error = QStringLiteral("Не удалось сохранить текущую базу перед заменой: %1")
                                .arg(problem);
            return outcome;
        }
    }

    QString problem;
    if (!Storage::removeDatabase(database, &problem)) {
        outcome.error = QStringLiteral("Текущая база занята: %1. Сервер точно остановлен?").arg(problem);
        return outcome;
    }
    if (!Storage::copyDatabase(backup, database, &problem)) {
        Storage::removeDatabase(database, nullptr);
        if (!outcome.safetyCopy.isEmpty())
            QFile::copy(outcome.safetyCopy, database);
        outcome.error = QStringLiteral("Не удалось восстановить из копии: %1. Прежняя база "
                                       "возвращена на место.")
                            .arg(problem);
        return outcome;
    }

    // The schema of this version, as the server would bring it to on start.
    {
        Storage storage;
        if (!storage.open(database, &problem)) {
            outcome.error = QStringLiteral("Восстановленная база не открывается: %1").arg(problem);
            return outcome;
        }
    }

    {
        Connection base(database, false);
        if (base.isOpen()) {
            QSqlQuery keep(base.db());
            keep.prepare(QStringLiteral("UPDATE users SET revision = :revision"
                                        " WHERE id = :id AND login_key = :key AND revision < :revision"));
            for (auto it = revisions.cbegin(); it != revisions.cend(); ++it) {
                keep.bindValue(QStringLiteral(":revision"), it.value().second);
                keep.bindValue(QStringLiteral(":id"), it.key());
                keep.bindValue(QStringLiteral(":key"), it.value().first);
                keep.exec();
            }
        }
    }

    outcome.ok = true;
    outcome.users = info.users;
    outcome.records = info.records;
    return outcome;
}
