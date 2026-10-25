#include "records.h"

#include <QJsonArray>

namespace {

struct KindNames
{
    const char *kind;
    const char *one;
    const char *many;
};

// The kinds of RecordKind in the planner (core/datastore.h there).
const KindNames kKinds[] = {
    { "calendar", "Календарь", "Календари" },
    { "event", "Событие", "События" },
    { "task", "Задача", "Задачи" },
    { "completion", "Выполнение задачи", "Журнал выполнений" },
    { "habit", "Привычка", "Привычки" },
    { "habitmark", "Отметка привычки", "Отметки привычек" },
    { "reminder", "Напоминание", "Напоминания" },
    { "melody", "Композиция", "Композиции" },
    { "alarmstop", "Выключенный будильник", "Выключенные будильники" },
};

const KindNames *namesOf(const QString &kind)
{
    for (const KindNames &names : kKinds) {
        if (kind == QLatin1String(names.kind))
            return &names;
    }
    return nullptr;
}

QString text(const QJsonObject &data, const char *key)
{
    return data.value(QLatin1String(key)).toString().trimmed();
}

QDateTime dateTime(const QJsonObject &data, const char *key)
{
    return QDateTime::fromString(text(data, key), Qt::ISODate);
}

} // namespace

QStringList PlannerRecord::kinds()
{
    QStringList result;
    for (const KindNames &names : kKinds)
        result << QLatin1String(names.kind);
    return result;
}

QString PlannerRecord::kindName(const QString &kind)
{
    const KindNames *names = namesOf(kind);
    return names ? QString::fromUtf8(names->one) : kind;
}

QString PlannerRecord::kindPlural(const QString &kind)
{
    const KindNames *names = namesOf(kind);
    return names ? QString::fromUtf8(names->many) : kind;
}

QString PlannerRecord::title(const QString &kind, const QJsonObject &data)
{
    if (kind == QLatin1String("calendar"))
        return text(data, "name");
    if (kind == QLatin1String("completion"))
        return text(data, "taskTitle");
    if (kind == QLatin1String("habitmark"))
        return text(data, "day");
    if (kind == QLatin1String("alarmstop"))
        return text(data, "itemId");
    return text(data, "title");
}

QString PlannerRecord::calendarId(const QJsonObject &data)
{
    return text(data, "calendarId");
}

QDateTime PlannerRecord::moment(const QString &kind, const QJsonObject &data, bool *allDay)
{
    if (allDay)
        *allDay = false;
    if (kind == QLatin1String("event")) {
        if (allDay)
            *allDay = data.value(QStringLiteral("allDay")).toBool();
        return dateTime(data, "start");
    }
    if (kind == QLatin1String("task")) {
        if (allDay)
            *allDay = !data.value(QStringLiteral("hasDueTime")).toBool();
        return dateTime(data, "dueAt");
    }
    if (kind == QLatin1String("reminder"))
        return dateTime(data, "at");
    if (kind == QLatin1String("completion"))
        return dateTime(data, "completedAt");
    if (kind == QLatin1String("alarmstop"))
        return dateTime(data, "fireAt");
    if (kind == QLatin1String("habit"))
        return QDateTime(QDate::fromString(text(data, "startedAt"), Qt::ISODate), QTime(0, 0));
    if (kind == QLatin1String("habitmark")) {
        if (allDay)
            *allDay = true;
        return QDateTime(QDate::fromString(text(data, "day"), Qt::ISODate), QTime(0, 0));
    }
    return QDateTime();
}

QString PlannerRecord::momentText(const QString &kind, const QJsonObject &data)
{
    bool allDay = false;
    const QDateTime at = moment(kind, data, &allDay);
    if (!at.isValid())
        return QString();
    // The planner keeps its moments in local time without a zone, and so they
    // are shown: the way the owner of the planner sees them.
    return allDay || kind == QLatin1String("habit")
               ? at.date().toString(QStringLiteral("dd.MM.yyyy"))
               : at.toString(QStringLiteral("dd.MM.yyyy HH:mm"));
}

QString PlannerRecord::state(const QString &kind, const QJsonObject &data)
{
    if (kind == QLatin1String("task")) {
        if (data.value(QStringLiteral("done")).toBool())
            return QStringLiteral("выполнена");
        if (data.value(QStringLiteral("failed")).toBool())
            return QStringLiteral("не выполнена");
        const int subtasks = data.value(QStringLiteral("subtasks")).toArray().size();
        return subtasks ? QStringLiteral("подзадач: %1").arg(subtasks) : QString();
    }
    if (kind == QLatin1String("event") && data.value(QStringLiteral("cancelled")).toBool())
        return QStringLiteral("отменено");
    if (kind == QLatin1String("reminder") && !data.value(QStringLiteral("enabled")).toBool(true))
        return QStringLiteral("выключено");
    if (kind == QLatin1String("habit") && !text(data, "archivedAt").isEmpty())
        return QStringLiteral("в архиве");
    if (kind == QLatin1String("completion") && data.value(QStringLiteral("failed")).toBool())
        return QStringLiteral("не выполнена");
    if (kind == QLatin1String("calendar") && !data.value(QStringLiteral("visible")).toBool(true))
        return QStringLiteral("скрыт");
    return QString();
}
