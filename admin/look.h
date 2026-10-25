#pragma once

#include <QColor>
#include <QDateTime>
#include <QString>

class QApplication;
class QFrame;
class QGridLayout;
class QLabel;
class QWidget;

/*!
 * The look of the admin panel in one place: its colors, the style sheet and
 * the small pieces every page is built of, plus how numbers, sizes and
 * moments are written for people.
 *
 * The colors of text, lines and surfaces are the neutral steps of the chart
 * palette (the series colors live with the chart, chart.cpp); the accent is the
 * violet of the planner's logo.
 */
namespace Look {

QColor plane();       //!< the window behind the cards
QColor surface();     //!< cards and charts
QColor sidebar();
QColor ink();         //!< primary text
QColor secondary();   //!< labels, captions
QColor muted();       //!< axis text, hints
QColor grid();        //!< hairlines
QColor baseline();
QColor accent();
QColor good();        //!< status: works
QColor warning();     //!< status: in between
QColor critical();    //!< status: broken

/*! Fusion with the palette and the style sheet of the panel. */
void apply(QApplication &app);

/*! A white card with a hairline border, the frame of every block of a page. */
QFrame *card(QWidget *parent = nullptr);

/*! The heading inside a card. */
QLabel *cardTitle(const QString &text, QWidget *parent = nullptr);

/*! A line of secondary text that wraps. */
QLabel *caption(const QString &text, QWidget *parent = nullptr);

/*!
 * Adds a row "label: value" to a two-column grid and returns the value, which
 * can be selected and copied: paths and addresses are copied out of it.
 */
QLabel *addRow(QGridLayout *grid, const QString &label, const QString &value = QString());

/*! A dot of @p color before @p text, as rich text for a QLabel. */
QString dot(const QColor &color, const QString &text);

/*! "1 284" with a narrow space, as Russian text writes thousands. */
QString number(qint64 value);

/*! "4,1 МБ". */
QString bytes(qint64 value);

/*! "25.10.2026 19:40" in local time, "—" for an invalid moment. */
QString moment(const QDateTime &value);

/*! "5 мин назад", "вчера", or the moment itself when it is older than a week. */
QString ago(const QDateTime &value, const QDateTime &now = QDateTime::currentDateTime());

/*! "2 ч 15 мин", "3 дн 4 ч": how long since @p since. */
QString span(const QDateTime &since, const QDateTime &now = QDateTime::currentDateTime());

/*! The Russian plural: plural(5, "аккаунт", "аккаунта", "аккаунтов"). */
QString plural(qint64 n, const QString &one, const QString &few, const QString &many);

} // namespace Look
