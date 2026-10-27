#pragma once

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>

/*!
 * Keys of one group of an INI file, read and written by hand: QSettings
 * would write the file anew and drop its comments, and service.ini of the
 * server explains every key in them.
 */
namespace IniFile {

/*! The keys of [@p group] of @p path; an absent file or group gives nothing. */
QHash<QString, QString> read(const QString &path, const QString &group);

/*!
 * Sets @p values in [@p group] and leaves everything else of the file as it
 * is - comments, other keys, other groups, its line endings: a key that is
 * there gets the new value in its own line, one that is not goes at the end
 * of the group, a group that is not goes at the end of the file. A missing
 * file is created.
 */
bool write(const QString &path, const QString &group, const QList<QPair<QString, QString>> &values,
           QString *error = nullptr);

} // namespace IniFile
