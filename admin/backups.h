#pragma once

#include "storage.h"

#include <QDateTime>
#include <QList>
#include <QString>

/*! One file of the folder of backups. */
struct BackupFile
{
    QString path;
    QDateTime modified;
    qint64 size = 0;
    DatabaseInfo info;   //!< what the copy holds; info.ok false when it does not open
};

/*! What a restore did, for the message the panel shows after it. */
struct RestoreOutcome
{
    bool ok = false;
    QString error;
    QString safetyCopy;  //!< where the base as it was before went, empty when there was none
    int users = 0;
    int records = 0;
};

/*!
 * Backups of the base: copies made with VACUUM INTO, which are consistent
 * snapshots even of a base the server is working on, and the way back from
 * one of them. The panel decides when; the files are done here.
 */
namespace Backups {

/*! "server-2026-10-25_19-40-12.db": sorts by the moment it was taken. */
QString fileName(const QDateTime &moment, const QString &prefix = QStringLiteral("server"));

/*! The bases in @p folder, the newest first. */
QList<BackupFile> list(const QString &folder);

/*! Copies @p database into @p folder; @p created gets the path of the copy. */
bool create(const QString &database, const QString &folder, QString *created, QString *error);

/*!
 * Puts @p backup in place of @p database. Nobody may hold the base open: the
 * server has to be stopped and the panel's own connection closed.
 *
 * The base as it was is copied into @p safetyFolder first (as is, if it is too
 * broken to be copied the proper way), and comes back if the restore fails
 * half way. The revision counter of every account is then kept at least where
 * it was: the devices hold cursors up to it, and a counter that went back
 * would hide from them every change made after the restore.
 */
RestoreOutcome restore(const QString &backup, const QString &database, const QString &safetyFolder);

} // namespace Backups
