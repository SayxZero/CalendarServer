#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

/*!
 * How the admin panel reads the records of a planner. The server keeps them as
 * opaque JSON, and so does the panel - except for the few fields it shows in
 * its lists: the title, the calendar and the moment of a record. They follow
 * the backup format of the planner (JsonIo there). A field the planner renames
 * shows up here as an empty column; the record itself can always be opened
 * whole, as the JSON it is.
 */
namespace PlannerRecord {

/*! The kinds the planner syncs, in the order the panel lists them. */
QStringList kinds();

/*! "Событие" for "event"; an unknown kind is shown as it is. */
QString kindName(const QString &kind);

/*! "События" for "event", for the headings of lists. */
QString kindPlural(const QString &kind);

/*! What the record is called: a calendar's name, a task's title and so on. */
QString title(const QString &kind, const QJsonObject &data);

/*! The calendar a record lives in; empty for the kinds that live in none. */
QString calendarId(const QJsonObject &data);

/*!
 * The moment the record is about: the start of an event, the due date of a
 * task, the hour of a reminder. Invalid when it has none.
 */
QDateTime moment(const QString &kind, const QJsonObject &data, bool *allDay = nullptr);

/*! moment() for people: "25.10.2026 09:00", or the date alone for a whole day. */
QString momentText(const QString &kind, const QJsonObject &data);

/*! A word on the state: "выполнена", "отменено", "выключено"; empty when plain. */
QString state(const QString &kind, const QJsonObject &data);

} // namespace PlannerRecord
