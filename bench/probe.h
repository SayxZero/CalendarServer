#pragma once

#include <QJsonObject>
#include <QString>

#include <atomic>
#include <functional>

/*!
 * What the machine of the server can do, measured where the server keeps
 * its base: the processor (cores, the cost of a password hash, whether the
 * cores really work at once), the memory and its limit, the disk (how long
 * a commit of SQLite takes with fsync and without it, how fast a file is
 * written) and the free space.
 */
struct SystemInfo
{
    int cores = 0;
    qint64 memoryTotal = -1;       //!< bytes of RAM
    qint64 memoryLimit = -1;       //!< what this process may have (job object, cgroup)
    double hashMs = -1;            //!< one PBKDF2 of the server (120 000 rounds), one thread
    double hashParallel = -1;      //!< how many hashes at once the cores make, against one
    double commitMs = -1;          //!< a commit of one row, synchronous = FULL
    double commitNormalMs = -1;    //!< the same, synchronous = NORMAL
    double writeMBps = -1;         //!< a 64 MB file written and synced
    qint64 freeBytes = -1;         //!< on the disk of the folder
    QString folder;

    /*! A commit with fsync over 3 ms: a hard disk, or a disk that syncs slowly. */
    bool slowDisk() const { return commitMs > 3; }

    QJsonObject toJson() const;
    static SystemInfo fromJson(const QJsonObject &object);

    /*! A few lines for people, as the console and the admin panel show them. */
    QStringList describe() const;
};

using ProbeProgress = std::function<void(double fraction, const QString &message)>;

/*! Measures it all in @p folder (it gets a few files that are removed after). */
SystemInfo probeSystem(const QString &folder, const std::atomic<bool> *cancel,
                       const ProbeProgress &progress = ProbeProgress());
