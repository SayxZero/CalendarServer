#include "probe.h"

#include "api.h"
#include "config.h"
#include "metrics.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QLocale>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QThread>
#include <QUuid>

#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

/*! The data of the file on the disk itself, not in the cache of the system. */
bool syncFile(QFile &file)
{
    if (!file.flush())
        return false;
#ifdef Q_OS_WIN
    return FlushFileBuffers(HANDLE(_get_osfhandle(file.handle()))) != 0;
#else
    return fsync(file.handle()) == 0;
#endif
}

/*! Milliseconds a commit of one row takes, on a fresh base of its own in WAL. */
double commitTime(const QString &folder, bool full, const std::atomic<bool> *cancel)
{
    const QString path = QDir(folder).filePath(QStringLiteral("probe-%1.db").arg(full ? "full" : "normal"));
    for (const char *suffix : { "", "-wal", "-shm" })
        QFile::remove(path + QLatin1String(suffix));
    const QString name = QStringLiteral("probe-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    double result = -1;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery query(db);
            query.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
            query.exec(full ? QStringLiteral("PRAGMA synchronous = FULL")
                            : QStringLiteral("PRAGMA synchronous = NORMAL"));
            query.exec(QStringLiteral("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)"));
            QSqlQuery insert(db);
            insert.prepare(QStringLiteral("INSERT INTO t (v) VALUES (:v)"));
            QElapsedTimer timer;
            timer.start();
            int done = 0;
            // Forty commits, or four seconds on a disk that takes its time.
            while (done < 40 && timer.elapsed() < 4000 && !(cancel && cancel->load())) {
                query.exec(QStringLiteral("BEGIN IMMEDIATE"));
                insert.bindValue(QStringLiteral(":v"), QString(200, QLatin1Char('x')));
                insert.exec();
                query.exec(QStringLiteral("COMMIT"));
                ++done;
            }
            if (done > 0)
                result = double(timer.nsecsElapsed()) / 1e6 / done;
            insert.finish();
            query.finish();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    for (const char *suffix : { "", "-wal", "-shm" })
        QFile::remove(path + QLatin1String(suffix));
    return result;
}

/*! MB a second a 64 MB file is written and synced with, stopping after ten seconds. */
double writeSpeed(const QString &folder, const std::atomic<bool> *cancel)
{
    const QString path = QDir(folder).filePath(QStringLiteral("probe-write.bin"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return -1;
    const QByteArray chunk(1024 * 1024, 'w');
    QElapsedTimer timer;
    timer.start();
    int written = 0;
    for (; written < 64 && timer.elapsed() < 10000 && !(cancel && cancel->load()); ++written) {
        if (file.write(chunk) != chunk.size())
            break;
    }
    syncFile(file);
    const double seconds = double(timer.nsecsElapsed()) / 1e9;
    file.close();
    QFile::remove(path);
    return seconds > 0 && written > 0 ? written / seconds : -1;
}

QString megabytes(qint64 bytes)
{
    return QLocale(QLocale::Russian).toString(double(bytes) / 1024 / 1024 / 1024, 'f', 1)
           + QStringLiteral(" ГБ");
}

} // namespace

QJsonObject SystemInfo::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("cores"), cores);
    object.insert(QStringLiteral("memoryTotal"), double(memoryTotal));
    object.insert(QStringLiteral("memoryLimit"), double(memoryLimit));
    object.insert(QStringLiteral("hashMs"), hashMs);
    object.insert(QStringLiteral("hashParallel"), hashParallel);
    object.insert(QStringLiteral("commitMs"), commitMs);
    object.insert(QStringLiteral("commitNormalMs"), commitNormalMs);
    object.insert(QStringLiteral("writeMBps"), writeMBps);
    object.insert(QStringLiteral("freeBytes"), double(freeBytes));
    object.insert(QStringLiteral("folder"), folder);
    return object;
}

SystemInfo SystemInfo::fromJson(const QJsonObject &object)
{
    SystemInfo info;
    info.cores = object.value(QStringLiteral("cores")).toInt();
    info.memoryTotal = qint64(object.value(QStringLiteral("memoryTotal")).toDouble(-1));
    info.memoryLimit = qint64(object.value(QStringLiteral("memoryLimit")).toDouble(-1));
    info.hashMs = object.value(QStringLiteral("hashMs")).toDouble(-1);
    info.hashParallel = object.value(QStringLiteral("hashParallel")).toDouble(-1);
    info.commitMs = object.value(QStringLiteral("commitMs")).toDouble(-1);
    info.commitNormalMs = object.value(QStringLiteral("commitNormalMs")).toDouble(-1);
    info.writeMBps = object.value(QStringLiteral("writeMBps")).toDouble(-1);
    info.freeBytes = qint64(object.value(QStringLiteral("freeBytes")).toDouble(-1));
    info.folder = object.value(QStringLiteral("folder")).toString();
    return info;
}

QStringList SystemInfo::describe() const
{
    const QLocale russian(QLocale::Russian);
    QStringList lines;
    lines << QStringLiteral("Процессор: %1 ядер, вход (хеш пароля) — %2 мс, ядра вместе быстрее одного в %3 раза")
                 .arg(cores)
                 .arg(qRound(hashMs))
                 .arg(russian.toString(hashParallel, 'f', 1));
    QString memory = QStringLiteral("Память: %1").arg(megabytes(memoryTotal));
    if (memoryLimit > 0 && memoryTotal > 0 && memoryLimit < memoryTotal)
        memory += QStringLiteral(", процессу можно %1").arg(megabytes(memoryLimit));
    lines << memory;
    lines << QStringLiteral("Диск: запись с fsync — %1 мс, без — %2 мс, файл пишется со скоростью %3 МБ/с%4")
                 .arg(russian.toString(commitMs, 'f', commitMs < 10 ? 2 : 0),
                      russian.toString(commitNormalMs, 'f', 2))
                 .arg(qRound(writeMBps))
                 .arg(slowDisk() ? QStringLiteral(" — медленный сброс на диск, похоже на жёсткий диск")
                                 : QString());
    if (freeBytes >= 0)
        lines << QStringLiteral("Свободно на диске: %1").arg(megabytes(freeBytes));
    return lines;
}

SystemInfo probeSystem(const QString &folder, const std::atomic<bool> *cancel,
                       const ProbeProgress &progress)
{
    const auto say = [&](double fraction, const QString &message) {
        if (progress)
            progress(fraction, message);
    };
    SystemInfo info;
    QDir().mkpath(folder);
    info.folder = QDir(folder).absolutePath();
    info.cores = qMax(1, QThread::idealThreadCount());
    ResourceProbe resources;
    info.memoryTotal = resources.sample().memoryTotal;
    info.memoryLimit = ServerConfig::memoryLimit();
    info.freeBytes = QStorageInfo(folder).bytesAvailable();

    say(0.05, QStringLiteral("хеш пароля на одном ядре"));
    QElapsedTimer timer;
    timer.start();
    Api::hashPassword(QStringLiteral("probe"), 120000);
    info.hashMs = double(timer.nsecsElapsed()) / 1e6;
    if (cancel && cancel->load())
        return info;

    say(0.2, QStringLiteral("хеши на всех ядрах сразу"));
    timer.restart();
    std::vector<std::thread> hashes;
    for (int i = 0; i < info.cores; ++i)
        hashes.emplace_back([] { Api::hashPassword(QStringLiteral("probe"), 120000); });
    for (std::thread &thread : hashes)
        thread.join();
    const double together = double(timer.nsecsElapsed()) / 1e6;
    info.hashParallel = together > 0 ? info.cores * info.hashMs / together : -1;
    if (cancel && cancel->load())
        return info;

    say(0.4, QStringLiteral("запись в базу с fsync"));
    info.commitMs = commitTime(folder, true, cancel);
    say(0.6, QStringLiteral("запись в базу без fsync"));
    info.commitNormalMs = commitTime(folder, false, cancel);
    say(0.75, QStringLiteral("скорость записи файла"));
    info.writeMBps = writeSpeed(folder, cancel);
    say(1, QStringLiteral("машина проверена"));
    return info;
}
