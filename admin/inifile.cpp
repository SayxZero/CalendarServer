#include "inifile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>

namespace {

/*! "[server]" -> "server"; empty for a line that is no group header. */
QString groupOf(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']')))
        return trimmed.mid(1, trimmed.size() - 2).trimmed();
    return QString();
}

/*! The key of a "key = value" line; empty for comments and the rest. */
QString keyOf(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char(';')) || trimmed.startsWith(QLatin1Char('#')))
        return QString();
    const int equals = trimmed.indexOf(QLatin1Char('='));
    return equals > 0 ? trimmed.left(equals).trimmed() : QString();
}

} // namespace

QHash<QString, QString> IniFile::read(const QString &path, const QString &group)
{
    QHash<QString, QString> values;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return values;
    bool inside = false;
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString header = groupOf(line);
        if (!header.isEmpty()) {
            inside = header.compare(group, Qt::CaseInsensitive) == 0;
            continue;
        }
        const QString key = keyOf(line);
        if (inside && !key.isEmpty())
            values.insert(key, line.mid(line.indexOf(QLatin1Char('=')) + 1).trimmed());
    }
    return values;
}

bool IniFile::write(const QString &path, const QString &group,
                    const QList<QPair<QString, QString>> &values, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    QByteArray raw;
    QFile existing(path);
    if (existing.exists()) {
        if (!existing.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("не удалось прочитать %1").arg(QDir::toNativeSeparators(path)));
        raw = existing.readAll();
        existing.close();
    }
    const bool crlf = raw.contains("\r\n");
    QStringList lines = QString::fromUtf8(raw).remove(QLatin1Char('\r')).split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();

    // Where the group is, and where it ends.
    int start = -1;
    int end = int(lines.size());
    for (int i = 0; i < lines.size(); ++i) {
        const QString header = groupOf(lines.at(i));
        if (header.isEmpty())
            continue;
        if (start >= 0) {
            end = i;
            break;
        }
        if (header.compare(group, Qt::CaseInsensitive) == 0)
            start = i;
    }
    if (start < 0) {
        if (!lines.isEmpty() && !lines.last().trimmed().isEmpty())
            lines << QString();
        lines << QStringLiteral("[%1]").arg(group);
        start = int(lines.size()) - 1;
        end = int(lines.size());
    }

    QList<QPair<QString, QString>> missing;
    for (const auto &value : values) {
        bool found = false;
        for (int i = start + 1; i < end; ++i) {
            if (keyOf(lines.at(i)).compare(value.first, Qt::CaseInsensitive) == 0) {
                lines[i] = QStringLiteral("%1=%2").arg(value.first, value.second);
                found = true;
            }
        }
        if (!found)
            missing << value;
    }
    // New keys after the last key of the group, not after its trailing blank lines.
    int insertAt = end;
    while (insertAt > start + 1 && lines.at(insertAt - 1).trimmed().isEmpty())
        --insertAt;
    for (const auto &value : missing)
        lines.insert(insertAt++, QStringLiteral("%1=%2").arg(value.first, value.second));

    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("не удалось записать %1").arg(QDir::toNativeSeparators(path)));
    const QString eol = crlf ? QStringLiteral("\r\n") : QStringLiteral("\n");
    file.write((lines.join(eol) + eol).toUtf8());
    if (!file.commit())
        return fail(QStringLiteral("не удалось записать %1").arg(QDir::toNativeSeparators(path)));
    return true;
}
