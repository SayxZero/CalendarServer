#include "runtimefile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace {

/*!
 * Whether process @p pid exists. A process of another account (the service
 * runs as LocalService) may refuse to be opened - it exists all the same.
 */
bool processAlive(qint64 pid)
{
    if (pid <= 0)
        return false;
#ifdef Q_OS_WIN
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!process)
        return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
#else
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

} // namespace

RuntimeFile::RuntimeFile(const QString &dataDir)
    : m_dataDir(dataDir)
{
}

RuntimeFile::~RuntimeFile()
{
    release();
}

QString RuntimeFile::lockPath(const QString &dataDir)
{
    return QDir(dataDir).absoluteFilePath(QStringLiteral("server.lock"));
}

QString RuntimeFile::infoPath(const QString &dataDir)
{
    return QDir(dataDir).absoluteFilePath(QStringLiteral("server.run"));
}

bool RuntimeFile::claim(QString *error)
{
    if (m_lock && m_lock->isLocked())
        return true;
    QDir().mkpath(m_dataDir);
    m_lock = std::make_unique<QLockFile>(lockPath(m_dataDir));
    // Never stale by age: a server runs for months, and a lock older than the
    // thirty seconds Qt assumes would be taken from under it. A lock is stale
    // only when its process is gone, which QLockFile checks by itself.
    m_lock->setStaleLockTime(0);
    if (m_lock->tryLock(0))
        return true;

    if (error) {
        qint64 pid = 0;
        QString host;
        QString application;
        if (m_lock->getLockInfo(&pid, &host, &application)) {
            *error = QStringLiteral("с этой базой уже работает другой сервер (процесс %1)").arg(pid);
        } else {
            *error = QStringLiteral("не удалось занять %1")
                         .arg(QDir::toNativeSeparators(lockPath(m_dataDir)));
        }
    }
    m_lock.reset();
    return false;
}

void RuntimeFile::publish(const RuntimeInfo &info)
{
    QJsonObject object;
    object.insert(QStringLiteral("pid"), double(info.pid ? info.pid
                                                         : QCoreApplication::applicationPid()));
    object.insert(QStringLiteral("port"), int(info.port));
    object.insert(QStringLiteral("host"), info.host);
    object.insert(QStringLiteral("logFile"), info.logFile);
    object.insert(QStringLiteral("version"), info.version);
    object.insert(QStringLiteral("startedAt"),
                  (info.startedAt.isValid() ? info.startedAt : QDateTime::currentDateTimeUtc())
                      .toUTC()
                      .toString(Qt::ISODate));
    object.insert(QStringLiteral("service"), info.service);

    QSaveFile file(infoPath(m_dataDir));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(object).toJson());
        file.commit();
    }
}

void RuntimeFile::release()
{
    if (!m_lock)
        return;
    QFile::remove(infoPath(m_dataDir));
    m_lock->unlock();
    m_lock.reset();
}

RuntimeInfo RuntimeFile::read(const QString &dataDir)
{
    RuntimeInfo info;
    QFile file(infoPath(dataDir));
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        info.pid = qint64(object.value(QStringLiteral("pid")).toDouble());
        info.port = quint16(object.value(QStringLiteral("port")).toInt());
        info.host = object.value(QStringLiteral("host")).toString();
        info.logFile = object.value(QStringLiteral("logFile")).toString();
        info.version = object.value(QStringLiteral("version")).toString();
        info.startedAt = QDateTime::fromString(object.value(QStringLiteral("startedAt")).toString(),
                                               Qt::ISODate);
        info.service = object.value(QStringLiteral("service")).toBool();
    }

    // The lock says who holds the base; server.run of a server that crashed
    // stays behind and names a process that is no more.
    QLockFile lock(lockPath(dataDir));
    qint64 holder = 0;
    QString host;
    QString application;
    if (QFile::exists(lockPath(dataDir)) && lock.getLockInfo(&holder, &host, &application)
        && processAlive(holder)) {
        if (info.pid != holder) {
            // server.run is of another, earlier process: only the lock is true.
            info = RuntimeInfo();
            info.pid = holder;
        }
        info.running = true;
    }
    return info;
}
