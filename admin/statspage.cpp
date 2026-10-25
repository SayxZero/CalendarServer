#include "statspage.h"

#include "admindb.h"
#include "adminsession.h"
#include "chart.h"
#include "look.h"
#include "records.h"

#include <QButtonGroup>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

QTreeWidget *table(const QStringList &headers)
{
    auto *tree = new QTreeWidget;
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->setSelectionMode(QAbstractItemView::NoSelection);
    tree->setFocusPolicy(Qt::NoFocus);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int i = 1; i < headers.size(); ++i)
        tree->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    return tree;
}

QTreeWidgetItem *row(QTreeWidget *tree, const QStringList &cells)
{
    auto *item = new QTreeWidgetItem(tree, cells);
    for (int column = 1; column < cells.size(); ++column)
        item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

QString dayTitle(const QDate &day)
{
    const QLocale russian(QLocale::Russian, QLocale::Russia);
    return russian.toString(day, QStringLiteral("ddd, d MMMM yyyy"));
}

QString share(int part, int whole)
{
    if (whole <= 0)
        return QStringLiteral("аккаунтов нет");
    return QStringLiteral("%1% аккаунтов").arg(qRound(100.0 * part / whole));
}

/*! A card with a title, a "Таблица" switch and a chart that the switch trades for its table. */
QFrame *chartCard(const QString &title, const QString &caption, Chart *chart, QTreeWidget *rows,
                  QPushButton **toggle, QStackedWidget **stack, QLabel **titleLabel = nullptr)
{
    QFrame *card = Look::card();
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(8);
    auto *head = new QHBoxLayout;
    QLabel *heading = Look::cardTitle(title);
    if (titleLabel)
        *titleLabel = heading;
    head->addWidget(heading, 1);
    *toggle = new QPushButton(QStringLiteral("Таблица"));
    (*toggle)->setCheckable(true);
    head->addWidget(*toggle);
    layout->addLayout(head);
    layout->addWidget(Look::caption(caption));
    *stack = new QStackedWidget;
    (*stack)->addWidget(chart);
    (*stack)->addWidget(rows);
    (*stack)->setMinimumHeight(250);
    layout->addWidget(*stack, 1);
    QStackedWidget *pages = *stack;
    QObject::connect(*toggle, &QPushButton::toggled, pages,
                     [pages](bool on) { pages->setCurrentIndex(on ? 1 : 0); });
    return card;
}

} // namespace

StatsPage::StatsPage(AdminSession *session, QWidget *parent)
    : Page(QStringLiteral("Аналитика"), session, true, parent)
{
    setSubtitle(QStringLiteral("Аккаунты, их активность и место, которое занимает база."));

    // ---- the period, one row above everything it scopes ---------------------
    auto *filters = new QHBoxLayout;
    filters->setSpacing(6);
    auto *periodLabel = new QLabel(QStringLiteral("Период:"));
    periodLabel->setProperty("role", "rowLabel");
    filters->addWidget(periodLabel);
    m_period = new QButtonGroup(this);
    m_period->setExclusive(true);
    const QList<QPair<int, QString>> periods = { { 30, QStringLiteral("30 дней") },
                                                 { 90, QStringLiteral("90 дней") },
                                                 { 365, QStringLiteral("Год") } };
    for (const auto &period : periods) {
        auto *button = new QPushButton(period.second);
        button->setCheckable(true);
        button->setChecked(period.first == 30);
        m_period->addButton(button, period.first);
        filters->addWidget(button);
    }
    filters->addStretch(1);
    m_updated = new QLabel;
    m_updated->setProperty("role", "hint");
    filters->addWidget(m_updated);
    auto *refresh = new QPushButton(QStringLiteral("Обновить"));
    filters->addWidget(refresh);
    body()->addLayout(filters);

    m_tracking = Look::caption(QString());
    m_tracking->setProperty("role", "hint");
    m_tracking->hide();
    body()->addWidget(m_tracking);

    // ---- tiles ---------------------------------------------------------------
    m_tiles = new QGridLayout;
    m_tiles->setHorizontalSpacing(14);
    m_tiles->setVerticalSpacing(14);
    m_accounts = addTile(0, 0, QStringLiteral("Учётные записи"));
    m_day = addTile(0, 1, QStringLiteral("Активны за 24 часа"));
    m_week = addTile(0, 2, QStringLiteral("Активны за 7 дней"));
    m_month = addTile(0, 3, QStringLiteral("Активны за 30 дней"));
    m_syncs = addTile(1, 0, QStringLiteral("Обмены за 30 дней"));
    m_devices = addTile(1, 1, QStringLiteral("Устройства"));
    m_records = addTile(1, 2, QStringLiteral("Записи"));
    m_storage = addTile(1, 3, QStringLiteral("База на диске"));
    for (int column = 0; column < 4; ++column)
        m_tiles->setColumnStretch(column, 1);
    body()->addLayout(m_tiles);

    // ---- charts --------------------------------------------------------------
    m_active = new Chart(Chart::Form::Lines);
    m_activeRows = table({ QStringLiteral("День"), QStringLiteral("За день"),
                           QStringLiteral("За 7 дней"), QStringLiteral("За 30 дней") });
    body()->addWidget(chartCard(
        QStringLiteral("Активные пользователи"),
        QStringLiteral("Сколько аккаунтов синхронизировались хотя бы раз: в этот день, за 7 и за 30 "
                       "дней до него включительно."),
        m_active, m_activeRows, &m_activeTable, &m_activeStack));

    m_exchanges = new Chart(Chart::Form::Columns);
    m_exchangeRows = table({ QStringLiteral("Когда"), QStringLiteral("Обменов"),
                             QStringLiteral("Получено"), QStringLiteral("Отдано") });
    body()->addWidget(chartCard(QStringLiteral("Обмены по дням"),
                                QStringLiteral("Сколько раз устройства всех аккаунтов обменялись с "
                                               "сервером."),
                                m_exchanges, m_exchangeRows, &m_syncsTable, &m_syncsStack,
                                &m_syncsTitle));

    // ---- storage -------------------------------------------------------------
    QFrame *storage = Look::card();
    auto *storageLayout = new QVBoxLayout(storage);
    storageLayout->setContentsMargins(20, 16, 20, 16);
    storageLayout->setSpacing(10);
    storageLayout->addWidget(Look::cardTitle(QStringLiteral("Хранилище и общий доступ")));
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(1, 1);
    m_file = Look::addRow(grid, QStringLiteral("Файл базы"));
    m_payload = Look::addRow(grid, QStringLiteral("Записи планеров"));
    m_free = Look::addRow(grid, QStringLiteral("Свободно в файле"));
    m_traffic = Look::addRow(grid, QStringLiteral("Передано за 30 дней"));
    m_shares = Look::addRow(grid, QStringLiteral("Общие календари"));
    storageLayout->addLayout(grid);
    auto *tables = new QHBoxLayout;
    tables->setSpacing(14);
    m_kinds = table({ QStringLiteral("Вид записей"), QStringLiteral("Записей"), QStringLiteral("Объём") });
    m_top = table({ QStringLiteral("Аккаунт"), QStringLiteral("Объём записей") });
    for (QTreeWidget *tree : { m_kinds, m_top })
        tree->setMinimumHeight(230);
    tables->addWidget(m_kinds, 3);
    tables->addWidget(m_top, 2);
    storageLayout->addLayout(tables);
    body()->addWidget(storage);
    body()->addStretch(1);

    connect(m_period, &QButtonGroup::idClicked, this, &StatsPage::reload);
    connect(refresh, &QPushButton::clicked, this, &StatsPage::reload);
    connect(session, &AdminSession::databaseChanged, this, &StatsPage::reload);
    reload();
}

StatsPage::Tile StatsPage::addTile(int row, int column, const QString &label)
{
    QFrame *card = Look::card();
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 12, 16, 14);
    layout->setSpacing(2);
    auto *name = new QLabel(label);
    name->setProperty("role", "tileLabel");
    Tile tile;
    tile.value = new QLabel(QStringLiteral("—"));
    tile.value->setProperty("role", "tileValue");
    tile.note = new QLabel;
    tile.note->setProperty("role", "tileNote");
    tile.note->setWordWrap(true);
    layout->addWidget(name);
    layout->addWidget(tile.value);
    layout->addWidget(tile.note);
    layout->addStretch(1);
    m_tiles->addWidget(card, row, column);
    return tile;
}

int StatsPage::days() const
{
    return qMax(30, m_period->checkedId());
}

void StatsPage::activate()
{
    reload();
}

void StatsPage::showTables(bool tables)
{
    m_activeTable->setChecked(tables);
    m_syncsTable->setChecked(tables);
}

void StatsPage::reload()
{
    AdminDb &db = session()->db();
    QString error;
    if (!session()->ensureDatabase(&error)) {
        m_tracking->setText(QStringLiteral("База не открыта: %1").arg(error));
        m_tracking->show();
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    const ServerTotals totals = db.totals(now);
    const StorageUse use = db.storage();
    const QList<DayActivity> history = db.history(days(), now.date());
    m_updated->setText(QStringLiteral("на %1").arg(now.toString(QStringLiteral("HH:mm"))));

    if (!totals.tracked) {
        m_tracking->setText(QStringLiteral("Эта база ещё не ведёт счёт обменов: его начинает сервер "
                                           "версии 1.1, открыв её. Активность пока неизвестна."));
    } else if (totals.trackedSince.isValid() && totals.trackedSince > now.date().addDays(-days())) {
        m_tracking->setText(QStringLiteral("Обмены считаются с %1. Раньше этого дня сервер знал "
                                           "только, когда каждое устройство заходило в последний раз.")
                                .arg(totals.trackedSince.toString(QStringLiteral("dd.MM.yyyy"))));
    } else {
        m_tracking->clear();
    }
    m_tracking->setVisible(!m_tracking->text().isEmpty());

    // ---- tiles ---------------------------------------------------------------
    const QString unknown = QStringLiteral("—");
    m_accounts.value->setText(Look::number(totals.accounts));
    m_accounts.note->setText(QStringLiteral("+%1 за 30 дней").arg(Look::number(totals.newAccounts)));
    const auto active = [&](Tile &tile, int value) {
        tile.value->setText(totals.tracked ? Look::number(value) : unknown);
        tile.note->setText(totals.tracked ? share(value, totals.accounts) : QString());
    };
    active(m_day, totals.activeDay);
    active(m_week, totals.activeWeek);
    active(m_month, totals.activeMonth);
    m_syncs.value->setText(totals.tracked ? Look::number(totals.syncsMonth) : unknown);
    m_syncs.note->setText(totals.tracked ? QStringLiteral("сегодня %1 · за 7 дней %2")
                                               .arg(Look::number(totals.syncsDay),
                                                    Look::number(totals.syncsWeek))
                                         : QString());
    m_devices.value->setText(Look::number(totals.devices));
    m_devices.note->setText(QStringLiteral("за 30 дней заходили %1").arg(Look::number(totals.devicesMonth)));
    int events = 0;
    int tasks = 0;
    for (const auto &kind : totals.recordsByKind) {
        if (kind.first == QLatin1String("event"))
            events = kind.second;
        else if (kind.first == QLatin1String("task"))
            tasks = kind.second;
    }
    m_records.value->setText(Look::number(totals.records));
    m_records.note->setText(QStringLiteral("событий %1 · задач %2").arg(Look::number(events),
                                                                         Look::number(tasks)));
    m_storage.value->setText(Look::bytes(use.file + use.wal));
    m_storage.note->setText(QStringLiteral("из них записи %1").arg(Look::bytes(use.payload)));

    // ---- active accounts -----------------------------------------------------
    QStringList ticks;
    QStringList titles;
    Chart::Series daily { QStringLiteral("за день"), Chart::seriesColor(0), {} };
    Chart::Series weekly { QStringLiteral("за 7 дней"), Chart::seriesColor(1), {} };
    Chart::Series monthly { QStringLiteral("за 30 дней"), Chart::seriesColor(2), {} };
    m_activeRows->clear();
    for (const DayActivity &point : history) {
        ticks << point.day.toString(QStringLiteral("dd.MM"));
        titles << dayTitle(point.day);
        daily.values << point.daily;
        weekly.values << point.weekly;
        monthly.values << point.monthly;
    }
    for (int i = int(history.size()) - 1; i >= 0; --i) {
        const DayActivity &point = history.at(i);
        row(m_activeRows, { point.day.toString(QStringLiteral("dd.MM.yyyy")), Look::number(point.daily),
                            Look::number(point.weekly), Look::number(point.monthly) });
    }
    m_active->setEmptyText(totals.tracked ? QStringLiteral("За этот период никто не синхронизировался")
                                          : QStringLiteral("База не ведёт счёт обменов"));
    m_active->setData(ticks, titles, { daily, weekly, monthly });

    // ---- exchanges: by day, or by week for a year ---------------------------
    const bool weeks = days() > 90;
    m_syncsTitle->setText(weeks ? QStringLiteral("Обмены по неделям") : QStringLiteral("Обмены по дням"));
    QStringList barTicks;
    QStringList barTitles;
    Chart::Series syncs { QStringLiteral("обменов"), Chart::seriesColor(0), {} };
    m_exchangeRows->clear();
    struct Bucket { QDate first; QDate last; int syncs = 0; qint64 in = 0; qint64 out = 0; };
    QList<Bucket> buckets;
    for (const DayActivity &point : history) {
        // A week starts on Monday, as the planner's do.
        if (!weeks || buckets.isEmpty() || point.day.dayOfWeek() == 1)
            buckets.append(Bucket { point.day, point.day });
        Bucket &bucket = buckets.last();
        bucket.last = point.day;
        bucket.syncs += point.syncs;
        bucket.in += point.bytesIn;
        bucket.out += point.bytesOut;
    }
    for (const Bucket &bucket : buckets) {
        barTicks << bucket.first.toString(QStringLiteral("dd.MM"));
        barTitles << (weeks ? QStringLiteral("%1 – %2").arg(bucket.first.toString(QStringLiteral("dd.MM")),
                                                             bucket.last.toString(QStringLiteral("dd.MM.yyyy")))
                            : dayTitle(bucket.first));
        syncs.values << bucket.syncs;
    }
    for (int i = int(buckets.size()) - 1; i >= 0; --i) {
        const Bucket &bucket = buckets.at(i);
        row(m_exchangeRows, { barTitles.at(i), Look::number(bucket.syncs), Look::bytes(bucket.in),
                              Look::bytes(bucket.out) });
    }
    m_exchanges->setEmptyText(totals.tracked ? QStringLiteral("За этот период обменов не было")
                                             : QStringLiteral("База не ведёт счёт обменов"));
    m_exchanges->setData(barTicks, barTitles, { syncs });

    // ---- storage -------------------------------------------------------------
    m_file->setText(QStringLiteral("%1 · журнал WAL %2 · страниц %3 по %4")
                        .arg(Look::bytes(use.file), Look::bytes(use.wal), Look::number(use.pages),
                             Look::bytes(use.pageSize)));
    m_payload->setText(QStringLiteral("%1 в %2 %3 · надгробий удалённых %4")
                           .arg(Look::bytes(use.payload), Look::number(totals.records))
                           .arg(Look::plural(totals.records, QStringLiteral("записи"),
                                             QStringLiteral("записях"), QStringLiteral("записях")))
                           .arg(Look::number(use.tombstones)));
    m_free->setText(QStringLiteral("%1 — освободится при сжатии базы")
                        .arg(Look::bytes(use.freePages * use.pageSize)));
    m_traffic->setText(totals.tracked
                           ? QStringLiteral("получено %1, отдано %2, принято изменений %3")
                                 .arg(Look::bytes(totals.bytesInMonth), Look::bytes(totals.bytesOutMonth),
                                      Look::number(totals.changesInMonth))
                           : unknown);
    m_shares->setText(QStringLiteral("открыто %1 · участников %2 · действующих ссылок %3")
                          .arg(Look::number(totals.sharedCalendars), Look::number(totals.shareMembers),
                               Look::number(totals.liveLinks)));

    m_kinds->clear();
    for (const StorageUse::Kind &kind : use.byKind)
        row(m_kinds, { PlannerRecord::kindPlural(kind.kind), Look::number(kind.count), Look::bytes(kind.bytes) });
    m_top->clear();
    for (const auto &top : use.topUsers)
        row(m_top, { top.first, Look::bytes(top.second) });
}
