#include "look.h"

#include <QApplication>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLocale>
#include <QPalette>
#include <QStyleFactory>

QColor Look::plane() { return QColor(0xf9, 0xf9, 0xf7); }
QColor Look::surface() { return QColor(0xfc, 0xfc, 0xfb); }
QColor Look::sidebar() { return QColor(0xf1, 0xf0, 0xec); }
QColor Look::ink() { return QColor(0x0b, 0x0b, 0x0b); }
QColor Look::secondary() { return QColor(0x52, 0x51, 0x4e); }
QColor Look::muted() { return QColor(0x89, 0x87, 0x81); }
QColor Look::grid() { return QColor(0xe1, 0xe0, 0xd9); }
QColor Look::baseline() { return QColor(0xc3, 0xc2, 0xb7); }
QColor Look::accent() { return QColor(0x61, 0x37, 0xc6); }
QColor Look::good() { return QColor(0x0c, 0xa3, 0x0c); }
QColor Look::warning() { return QColor(0xfa, 0xb2, 0x19); }
QColor Look::critical() { return QColor(0xd0, 0x3b, 0x3b); }

namespace {

const char *kStyleSheet = R"(
QMainWindow, QWidget#plane { background: #f9f9f7; }
QScrollArea { border: none; background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }

QFrame[card="true"] {
    background: #fcfcfb;
    border: 1px solid #e1e0d9;
    border-radius: 8px;
}
QLabel { background: transparent; }
QLabel[role="pageTitle"] { font-size: 16pt; font-weight: 600; color: #0b0b0b; }
QLabel[role="pageSubtitle"], QLabel[role="caption"], QLabel[role="rowLabel"],
QLabel[role="tileLabel"] { color: #52514e; }
QLabel[role="cardTitle"] { font-size: 11pt; font-weight: 600; color: #0b0b0b; }
QLabel[role="tileValue"] { font-size: 20pt; font-weight: 600; color: #0b0b0b; }
QLabel[role="tileNote"], QLabel[role="hint"] { color: #898781; }
QLabel[role="status"] { font-size: 13pt; font-weight: 600; color: #0b0b0b; }
QLabel[role="brand"] { font-size: 11pt; font-weight: 600; color: #0b0b0b; }
QLabel[role="problem"] {
    color: #8f2424;
    background: #fbeeee;
    border: 1px solid #f0caca;
    border-radius: 6px;
    padding: 8px 10px;
}

QWidget#sidebar { background: #f1f0ec; border-right: 1px solid #e1e0d9; }
QListWidget#nav { background: transparent; border: none; outline: 0; }
QListWidget#nav::item {
    padding: 8px 12px;
    margin: 1px 8px;
    border-radius: 6px;
    color: #0b0b0b;
}
QListWidget#nav::item:hover { background: #e8e6df; }
QListWidget#nav::item:selected { background: #e2dbf6; color: #3b1f8a; }

QPushButton {
    padding: 5px 14px;
    border: 1px solid #c3c2b7;
    border-radius: 6px;
    background: #fcfcfb;
    color: #0b0b0b;
    min-height: 18px;
}
QPushButton:hover { background: #f1f0ec; }
QPushButton:pressed { background: #e8e6df; }
QPushButton:disabled { color: #a8a7a0; border-color: #e1e0d9; background: #f9f9f7; }
QPushButton:checked { background: #e2dbf6; border-color: #6137c6; color: #3b1f8a; }
QPushButton[accent="true"] { background: #6137c6; border-color: #6137c6; color: #ffffff; }
QPushButton[accent="true"]:hover { background: #5430b0; border-color: #5430b0; }
QPushButton[accent="true"]:pressed { background: #48289a; }
QPushButton[accent="true"]:disabled { background: #cfc5ec; border-color: #cfc5ec; color: #ffffff; }
QPushButton[danger="true"] { color: #d03b3b; }
QPushButton[danger="true"]:disabled { color: #a8a7a0; }

QLineEdit {
    padding: 4px 8px;
    border: 1px solid #c3c2b7;
    border-radius: 6px;
    background: #fcfcfb;
    selection-background-color: #6137c6;
}
QLineEdit:focus { border-color: #6137c6; }

QTreeView, QTableView {
    background: #fcfcfb;
    alternate-background-color: #f6f5f2;
    border: 1px solid #e1e0d9;
    border-radius: 6px;
    selection-background-color: #e2dbf6;
    selection-color: #0b0b0b;
    outline: 0;
}
QTreeView::item, QTableView::item { padding: 3px 4px; }
QHeaderView::section {
    background: #f6f5f2;
    color: #52514e;
    border: none;
    border-bottom: 1px solid #e1e0d9;
    padding: 5px 8px;
}

QTabWidget::pane { border: none; }
QTabBar::tab {
    padding: 6px 12px;
    margin-right: 2px;
    border: none;
    border-bottom: 2px solid transparent;
    color: #52514e;
    background: transparent;
}
QTabBar::tab:hover { color: #0b0b0b; }
QTabBar::tab:selected { color: #0b0b0b; border-bottom: 2px solid #6137c6; }

QPlainTextEdit {
    background: #fcfcfb;
    border: 1px solid #e1e0d9;
    border-radius: 6px;
    selection-background-color: #6137c6;
}
QSplitter::handle { background: transparent; }
QStatusBar { background: #f1f0ec; color: #52514e; border-top: 1px solid #e1e0d9; }
QStatusBar::item { border: none; }
QToolTip { background: #fcfcfb; color: #0b0b0b; border: 1px solid #e1e0d9; padding: 4px; }
)";

QLocale russian()
{
    return QLocale(QLocale::Russian, QLocale::Russia);
}

} // namespace

void Look::apply(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette palette;
    palette.setColor(QPalette::Window, plane());
    palette.setColor(QPalette::WindowText, ink());
    palette.setColor(QPalette::Base, surface());
    palette.setColor(QPalette::AlternateBase, QColor(0xf6, 0xf5, 0xf2));
    palette.setColor(QPalette::Text, ink());
    palette.setColor(QPalette::Button, surface());
    palette.setColor(QPalette::ButtonText, ink());
    palette.setColor(QPalette::Highlight, accent());
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, surface());
    palette.setColor(QPalette::ToolTipText, ink());
    palette.setColor(QPalette::PlaceholderText, muted());
    palette.setColor(QPalette::Link, accent());
    palette.setColor(QPalette::Mid, grid());
    palette.setColor(QPalette::Disabled, QPalette::Text, muted());
    palette.setColor(QPalette::Disabled, QPalette::WindowText, muted());
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, muted());
    app.setPalette(palette);

    QFont font = app.font();
    if (font.pointSizeF() > 0 && font.pointSizeF() < 10)
        font.setPointSizeF(10);
    app.setFont(font);
    app.setStyleSheet(QString::fromUtf8(kStyleSheet));
}

QFrame *Look::card(QWidget *parent)
{
    auto *frame = new QFrame(parent);
    frame->setProperty("card", true);
    return frame;
}

QLabel *Look::cardTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setProperty("role", "cardTitle");
    return label;
}

QLabel *Look::caption(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setProperty("role", "caption");
    label->setWordWrap(true);
    return label;
}

QLabel *Look::addRow(QGridLayout *grid, const QString &label, const QString &value)
{
    const int row = grid->rowCount();
    auto *name = new QLabel(label);
    name->setProperty("role", "rowLabel");
    name->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto *text = new QLabel(value);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    text->setWordWrap(true);
    text->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    grid->addWidget(name, row, 0);
    grid->addWidget(text, row, 1);
    return text;
}

QString Look::dot(const QColor &color, const QString &text)
{
    return QStringLiteral("<span style=\"color:%1\">&#9679;</span>&nbsp;%2")
        .arg(color.name(), text.toHtmlEscaped());
}

QString Look::number(qint64 value)
{
    return russian().toString(value);
}

QString Look::bytes(qint64 value)
{
    if (value < 1024)
        return QStringLiteral("%1 Б").arg(value);
    const char *units[] = { "КБ", "МБ", "ГБ", "ТБ" };
    double size = double(value) / 1024.0;
    int unit = 0;
    while (size >= 1024.0 && unit < 3) {
        size /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2")
        .arg(russian().toString(size, 'f', size < 10 ? 1 : 0), QString::fromUtf8(units[unit]));
}

QString Look::moment(const QDateTime &value)
{
    return value.isValid() ? value.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm"))
                           : QStringLiteral("—");
}

QString Look::plural(qint64 n, const QString &one, const QString &few, const QString &many)
{
    const qint64 tens = n % 100;
    const qint64 ones = n % 10;
    if (tens >= 11 && tens <= 14)
        return many;
    if (ones == 1)
        return one;
    if (ones >= 2 && ones <= 4)
        return few;
    return many;
}

QString Look::ago(const QDateTime &value, const QDateTime &now)
{
    if (!value.isValid())
        return QStringLiteral("—");
    const QDateTime local = value.toLocalTime();
    const qint64 seconds = local.secsTo(now);
    if (seconds < 60)
        return QStringLiteral("только что");
    if (seconds < 3600) {
        const qint64 minutes = seconds / 60;
        return QStringLiteral("%1 мин назад").arg(minutes);
    }
    const QString time = local.toString(QStringLiteral("HH:mm"));
    if (local.date() == now.date())
        return QStringLiteral("сегодня в %1").arg(time);
    if (local.date() == now.date().addDays(-1))
        return QStringLiteral("вчера в %1").arg(time);
    const qint64 days = local.date().daysTo(now.date());
    if (days < 7)
        return QStringLiteral("%1 %2 назад").arg(days).arg(plural(days, QStringLiteral("день"),
                                                                   QStringLiteral("дня"),
                                                                   QStringLiteral("дней")));
    return moment(value);
}

QString Look::span(const QDateTime &since, const QDateTime &now)
{
    if (!since.isValid())
        return QStringLiteral("—");
    const qint64 seconds = qMax<qint64>(0, since.secsTo(now));
    const qint64 days = seconds / 86400;
    const qint64 hours = seconds % 86400 / 3600;
    const qint64 minutes = seconds % 3600 / 60;
    if (days > 0)
        return QStringLiteral("%1 дн %2 ч").arg(days).arg(hours);
    if (hours > 0)
        return QStringLiteral("%1 ч %2 мин").arg(hours).arg(minutes);
    return QStringLiteral("%1 мин").arg(qMax<qint64>(1, minutes));
}
