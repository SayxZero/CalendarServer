#include "loadpage.h"

#include "adminsession.h"
#include "chart.h"
#include "look.h"

#include <QButtonGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

namespace {

const QLocale &russian()
{
    static const QLocale locale(QLocale::Russian, QLocale::Russia);
    return locale;
}

/*! "0,4 мс", "12 мс", "1,3 с". */
QString milliseconds(double value)
{
    if (std::isnan(value) || value < 0)
        return QStringLiteral("—");
    if (value >= 1000)
        return russian().toString(value / 1000, 'f', 1) + QStringLiteral(" с");
    return russian().toString(value, 'f', value < 10 ? 1 : 0) + QStringLiteral(" мс");
}

/*! How long a thread has worked: "40 с", "12 мин", "3 ч 05 мин", "2 дн 4 ч". */
QString duration(double ms)
{
    if (std::isnan(ms) || ms < 0)
        return QStringLiteral("—");
    if (ms < 60 * 1000)
        return milliseconds(ms);
    const qint64 minutes = qint64(ms / 60000);
    if (minutes < 60)
        return QStringLiteral("%1 мин").arg(minutes);
    if (minutes < 24 * 60)
        return QStringLiteral("%1 ч %2 мин").arg(minutes / 60).arg(minutes % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1 дн %2 ч").arg(minutes / (24 * 60)).arg(minutes / 60 % 24);
}

QString percent(double value)
{
    if (std::isnan(value) || value < 0)
        return QStringLiteral("—");
    return russian().toString(value, 'f', value < 10 ? 1 : 0) + QStringLiteral(" %");
}

QString perMinute(double value)
{
    if (std::isnan(value))
        return QStringLiteral("—");
    return value < 10 ? russian().toString(value, 'f', 1) : Look::number(qRound64(value));
}

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

void row(QTreeWidget *tree, const QStringList &cells)
{
    auto *item = new QTreeWidgetItem(tree, cells);
    for (int column = 1; column < cells.size(); ++column)
        item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
}

/*! The label under the axis and the head of the tooltip for one bucket. */
QString bucketTick(const LoadPoint &point, LoadPeriod period)
{
    switch (period) {
    case LoadPeriod::Hour:
    case LoadPeriod::Day:
        return point.start.toString(QStringLiteral("HH:mm"));
    case LoadPeriod::Week:
    case LoadPeriod::Month:
    case LoadPeriod::Year:
        return point.start.toString(QStringLiteral("dd.MM"));
    }
    return QString();
}

QString bucketTitle(const LoadPoint &point, LoadPeriod period)
{
    const QDateTime end = point.start.addSecs(point.seconds);
    if (period == LoadPeriod::Year)
        return russian().toString(point.start, QStringLiteral("ddd, d MMMM yyyy"));
    return QStringLiteral("%1 – %2").arg(russian().toString(point.start, QStringLiteral("d MMMM, HH:mm")),
                                          end.toString(QStringLiteral("HH:mm")));
}

} // namespace

LoadPage::LoadPage(AdminSession *session, QWidget *parent)
    : Page(QStringLiteral("Нагрузка"), session, true, parent)
{
    setSubtitle(QStringLiteral("Запросы, очередь к потокам сервера, процессор и память — сейчас и "
                               "за прошедшее время. История хранится год."));

    // ---- the period and the view, one row above everything they scope -------
    auto *filters = new QHBoxLayout;
    filters->setSpacing(6);
    auto *periodLabel = new QLabel(QStringLiteral("Период:"));
    periodLabel->setProperty("role", "rowLabel");
    filters->addWidget(periodLabel);
    m_period = new QButtonGroup(this);
    m_period->setExclusive(true);
    const QList<QPair<LoadPeriod, QString>> periods = {
        { LoadPeriod::Hour, QStringLiteral("Час") },     { LoadPeriod::Day, QStringLiteral("Сутки") },
        { LoadPeriod::Week, QStringLiteral("Неделя") }, { LoadPeriod::Month, QStringLiteral("Месяц") },
        { LoadPeriod::Year, QStringLiteral("Год") },
    };
    for (const auto &item : periods) {
        auto *button = new QPushButton(item.second);
        button->setCheckable(true);
        button->setChecked(item.first == LoadPeriod::Hour);
        m_period->addButton(button, int(item.first));
        filters->addWidget(button);
    }
    filters->addSpacing(12);
    m_tableToggle = new QPushButton(QStringLiteral("Таблица"));
    m_tableToggle->setCheckable(true);
    filters->addWidget(m_tableToggle);
    filters->addStretch(1);
    m_updated = new QLabel;
    m_updated->setProperty("role", "hint");
    filters->addWidget(m_updated);
    auto *refresh = new QPushButton(QStringLiteral("Обновить"));
    filters->addWidget(refresh);
    body()->addLayout(filters);

    m_state = Look::caption(QString());
    body()->addWidget(m_state);

    // ---- now -----------------------------------------------------------------
    m_tiles = new QGridLayout;
    m_tiles->setHorizontalSpacing(14);
    m_tiles->setVerticalSpacing(14);
    m_requests = addTile(0, 0, QStringLiteral("Запросов в минуту"));
    m_queue = addTile(0, 1, QStringLiteral("В очереди к потокам"));
    m_wait = addTile(0, 2, QStringLiteral("Ожидание в очереди"));
    m_work = addTile(0, 3, QStringLiteral("Обработка запроса"));
    m_cpu = addTile(1, 0, QStringLiteral("Процессор"));
    m_memory = addTile(1, 1, QStringLiteral("Память"));
    m_connections = addTile(1, 2, QStringLiteral("Соединения"));
    m_refused = addTile(1, 3, QStringLiteral("Отказы из-за нагрузки"));
    for (int column = 0; column < 4; ++column)
        m_tiles->setColumnStretch(column, 1);
    body()->addLayout(m_tiles);

    // ---- history: charts, or all of it in one table ---------------------------
    m_views = new QStackedWidget;
    auto *charts = new QWidget;
    auto *chartsLayout = new QVBoxLayout(charts);
    chartsLayout->setContentsMargins(0, 0, 0, 0);
    chartsLayout->setSpacing(14);

    m_requestChart = new Chart(Chart::Form::Lines);
    m_requestChart->setValueFormat(1);
    chartsLayout->addWidget(chartCard(QStringLiteral("Запросы в минуту"),
                                      QStringLiteral("Среднее за каждую точку, пока сервер работал. "
                                                     "Отказы — ответы 503 «сервер перегружен»."),
                                      m_requestChart));
    m_queueChart = new Chart(Chart::Form::Lines);
    m_queueChart->setValueFormat(1);
    chartsLayout->addWidget(chartCard(QStringLiteral("Очередь к потокам"),
                                      QStringLiteral("Сколько запросов ждали свободного потока: в "
                                                     "среднем и наибольшая очередь."),
                                      m_queueChart));
    m_timeChart = new Chart(Chart::Form::Lines);
    m_timeChart->setValueFormat(1, QStringLiteral(" мс"));
    chartsLayout->addWidget(chartCard(QStringLiteral("Ожидание и обработка"),
                                      QStringLiteral("Среднее время запроса в очереди до потока и "
                                                     "время его обработки потоком."),
                                      m_timeChart));
    m_cpuChart = new Chart(Chart::Form::Lines);
    m_cpuChart->setValueFormat(1, QStringLiteral(" %"));
    chartsLayout->addWidget(chartCard(QStringLiteral("Процессор"),
                                      QStringLiteral("Доля всех ядер, занятая сервером и всей "
                                                     "системой."),
                                      m_cpuChart));
    m_memoryChart = new Chart(Chart::Form::Lines);
    m_memoryChart->setValueFormat(1, QStringLiteral(" МБ"));
    chartsLayout->addWidget(chartCard(QStringLiteral("Память сервера"),
                                      QStringLiteral("Сколько оперативной памяти занимает процесс "
                                                     "сервера."),
                                      m_memoryChart));
    m_connectionChart = new Chart(Chart::Form::Lines);
    m_connectionChart->setValueFormat(1);
    chartsLayout->addWidget(chartCard(QStringLiteral("Соединения и занятые потоки"),
                                      QStringLiteral("Открытые соединения клиентов и потоки, занятые "
                                                     "запросами, в среднем."),
                                      m_connectionChart));
    m_views->addWidget(charts);

    m_rows = table({ QStringLiteral("Когда"), QStringLiteral("Запр./мин"), QStringLiteral("Отказы"),
                     QStringLiteral("Очередь ср."), QStringLiteral("макс."),
                     QStringLiteral("Ожидание"), QStringLiteral("Обработка"),
                     QStringLiteral("Процессор"), QStringLiteral("Система"),
                     QStringLiteral("Память"), QStringLiteral("Соединения"),
                     QStringLiteral("Записей за сброс") });
    m_rows->setMinimumHeight(520);
    QFrame *tableCard = Look::card();
    auto *tableLayout = new QVBoxLayout(tableCard);
    tableLayout->setContentsMargins(20, 16, 20, 16);
    tableLayout->addWidget(Look::cardTitle(QStringLiteral("История нагрузки")));
    tableLayout->addWidget(m_rows, 1);
    m_views->addWidget(tableCard);
    body()->addWidget(m_views);

    // ---- the threads -----------------------------------------------------------
    QFrame *threads = Look::card();
    auto *threadsLayout = new QVBoxLayout(threads);
    threadsLayout->setContentsMargins(20, 16, 20, 16);
    threadsLayout->setSpacing(8);
    threadsLayout->addWidget(Look::cardTitle(QStringLiteral("Распределение по потокам")));
    m_threadNote = Look::caption(QString());
    threadsLayout->addWidget(m_threadNote);
    auto *threadRow = new QHBoxLayout;
    threadRow->setSpacing(14);
    m_threadChart = new Chart(Chart::Form::Columns);
    m_threadChart->setEmptyText(QStringLiteral("Сервер ещё не обработал ни одного запроса"));
    m_threadChart->setMinimumHeight(200);
    m_threadRows = table({ QStringLiteral("Поток"), QStringLiteral("Запросов"), QStringLiteral("Доля"),
                           QStringLiteral("Занят") });
    m_threadRows->setMinimumHeight(200);
    threadRow->addWidget(m_threadChart, 3);
    threadRow->addWidget(m_threadRows, 2);
    threadsLayout->addLayout(threadRow);
    body()->addWidget(threads);
    body()->addStretch(1);

    m_refresh = new QTimer(this);
    m_refresh->setInterval(5000);
    connect(m_refresh, &QTimer::timeout, this, &LoadPage::reload);
    connect(m_period, &QButtonGroup::idClicked, this, &LoadPage::reload);
    connect(refresh, &QPushButton::clicked, this, &LoadPage::reload);
    connect(m_tableToggle, &QPushButton::toggled, m_views,
            [this](bool on) { m_views->setCurrentIndex(on ? 1 : 0); });
    connect(session, &AdminSession::databaseChanged, this, &LoadPage::reload);
}

LoadPage::Tile LoadPage::addTile(int row, int column, const QString &label)
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

QFrame *LoadPage::chartCard(const QString &title, const QString &caption, Chart *chart)
{
    QFrame *card = Look::card();
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(8);
    layout->addWidget(Look::cardTitle(title));
    layout->addWidget(Look::caption(caption));
    chart->setMinimumHeight(220);
    layout->addWidget(chart, 1);
    return card;
}

LoadPeriod LoadPage::period() const
{
    return LoadPeriod(qMax(0, m_period->checkedId()));
}

void LoadPage::activate()
{
    reload();
    m_refresh->start();
}

void LoadPage::hideEvent(QHideEvent *event)
{
    // Nobody looks: no reason to read the file every five seconds.
    m_refresh->stop();
    Page::hideEvent(event);
}

void LoadPage::showTable(bool table)
{
    m_tableToggle->setChecked(table);
}

void LoadPage::reload()
{
    const QDateTime now = QDateTime::currentDateTime();
    m_updated->setText(QStringLiteral("на %1").arg(now.toString(QStringLiteral("HH:mm:ss"))));

    if (!m_history.isOpen()) {
        QString error;
        if (!m_history.open(session()->config().resolvedDataDir(), &error)) {
            m_state->setText(QStringLiteral("Нет данных о нагрузке: %1.").arg(error));
            showNow(LoadNow(), {});
            showHistory({});
            showThreads(LoadNow());
            return;
        }
    }

    const LoadNow load = m_history.now(now);
    const QList<LoadPoint> lastHour = m_history.points(LoadPeriod::Hour, now);
    const QList<LoadPoint> points =
        period() == LoadPeriod::Hour ? lastHour : m_history.points(period(), now);

    QString state;
    if (!load.known) {
        state = QStringLiteral("Сервер ещё не записал ни одного замера.");
    } else if (!load.running) {
        state = QStringLiteral("Сервер не запущен: последний замер %1. Ниже — история.")
                    .arg(Look::moment(load.at));
    } else {
        state = QStringLiteral("Сервер %1 работает %2 · потоков %3, очередь «%4» · замер раз в 5 с.")
                    .arg(load.version, Look::span(load.startedAt, now))
                    .arg(load.threads)
                    .arg(load.mode == QLatin1String("fair") ? QStringLiteral("справедливая")
                                                            : load.mode);
    }
    const QDateTime first = m_history.firstRecorded();
    if (first.isValid())
        state += QStringLiteral(" История с %1.").arg(first.toString(QStringLiteral("dd.MM.yyyy HH:mm")));
    m_state->setText(state);

    showNow(load, lastHour);
    showHistory(points);
    showThreads(load);
}

void LoadPage::showNow(const LoadNow &now, const QList<LoadPoint> &lastHour)
{
    const bool live = now.running;
    const QString none = QStringLiteral("—");

    m_requests.value->setText(live ? perMinute(now.requestsPerMinute) : none);
    m_requests.note->setText(now.known ? QStringLiteral("за последнюю минуту · всего с запуска %1")
                                             .arg(Look::number(now.requestsTotal))
                                       : QString());

    m_queue.value->setText(live ? Look::number(now.queued) : none);
    m_queue.note->setText(live ? QStringLiteral("наибольшая за минуту %1 · занято потоков %2 из %3")
                                     .arg(Look::number(now.queuedMax))
                                     .arg(now.busy)
                                     .arg(now.threads)
                               : QString());

    m_wait.value->setText(live ? milliseconds(now.waitAvgMs) : none);
    m_wait.note->setText(live ? QStringLiteral("в среднем за минуту · дольше всех %1")
                                    .arg(milliseconds(now.waitMaxMs))
                              : QString());

    m_work.value->setText(live ? milliseconds(now.workAvgMs) : none);
    m_work.note->setText(
        live ? QStringLiteral("в среднем за минуту · дольше всех %1 · запись в базу ждёт %2, "
                              "за один сброс на диск %3")
                   .arg(milliseconds(now.workMaxMs), milliseconds(now.gateWaitAvgMs),
                        now.writesPerCommit > 0
                            ? QStringLiteral("%1 %2")
                                  .arg(russian().toString(now.writesPerCommit, 'f', 1),
                                       Look::plural(qRound64(now.writesPerCommit),
                                                    QStringLiteral("операция"),
                                                    QStringLiteral("операции"),
                                                    QStringLiteral("операций")))
                            : QStringLiteral("—"))
             : QString());

    m_cpu.value->setText(live ? percent(now.cpuProcess) : none);
    m_cpu.note->setText(live ? QStringLiteral("сервер, от всех ядер · система занята на %1")
                                   .arg(percent(now.cpuSystem))
                             : QString());

    m_memory.value->setText(live && now.memoryProcess >= 0 ? Look::bytes(now.memoryProcess) : none);
    if (live && now.memoryTotal > 0 && now.memoryProcess >= 0) {
        m_memory.note->setText(QStringLiteral("%1 от %2 ОЗУ · в системе занято %3 · запросы держат %4 "
                                              "из %5")
                                   .arg(percent(100.0 * double(now.memoryProcess) / double(now.memoryTotal)),
                                        Look::bytes(now.memoryTotal), percent(now.memoryLoad),
                                        Look::bytes(now.buffered), Look::bytes(now.bufferBudget)));
    } else {
        m_memory.note->clear();
    }

    m_connections.value->setText(live ? Look::number(now.connections) : none);
    m_connections.note->setText(
        live ? (now.paused ? QStringLiteral("предел %1 достигнут: новые ждут очереди системы")
                           : QStringLiteral("предел %1"))
                   .arg(Look::number(now.connectionLimit))
             : QString());

    qint64 refusedHour = 0;
    for (const LoadPoint &point : lastHour)
        refusedHour += point.overloaded;
    m_refused.value->setText(now.known ? Look::number(refusedHour) : none);
    m_refused.note->setText(now.known ? QStringLiteral("ответов 503 за час · с запуска %1")
                                            .arg(Look::number(now.overloadedTotal))
                                      : QString());
}

void LoadPage::showHistory(const QList<LoadPoint> &points)
{
    const LoadPeriod shown = period();
    QStringList ticks;
    QStringList titles;
    Chart::Series requests { QStringLiteral("запросов"), Chart::seriesColor(0), {} };
    Chart::Series refused { QStringLiteral("отказов"), Chart::seriesColor(1), {} };
    Chart::Series queueAvg { QStringLiteral("в среднем"), Chart::seriesColor(0), {} };
    Chart::Series queueMax { QStringLiteral("наибольшая"), Chart::seriesColor(1), {} };
    Chart::Series wait { QStringLiteral("ожидание в очереди"), Chart::seriesColor(0), {} };
    Chart::Series work { QStringLiteral("обработка"), Chart::seriesColor(1), {} };
    Chart::Series cpuServer { QStringLiteral("сервер"), Chart::seriesColor(0), {} };
    Chart::Series cpuSystem { QStringLiteral("вся система"), Chart::seriesColor(1), {} };
    Chart::Series memory { QStringLiteral("память"), Chart::seriesColor(0), {} };
    Chart::Series connections { QStringLiteral("соединений"), Chart::seriesColor(0), {} };
    Chart::Series busy { QStringLiteral("занятых потоков"), Chart::seriesColor(1), {} };

    m_rows->clear();
    for (const LoadPoint &point : points) {
        ticks << bucketTick(point, shown);
        titles << bucketTitle(point, shown);
        requests.values << point.requestsPerMinute;
        refused.values << point.overloadedPerMinute;
        queueAvg.values << point.queueAvg;
        queueMax.values << point.queueMax;
        wait.values << point.waitAvgMs;
        work.values << point.workAvgMs;
        cpuServer.values << point.cpuProcess;
        cpuSystem.values << point.cpuSystem;
        memory.values << point.memoryMb;
        connections.values << point.connectionsAvg;
        busy.values << point.busyAvg;
    }
    // The table: the newest first, and only the buckets the server ran in.
    for (int i = int(points.size()) - 1; i >= 0; --i) {
        const LoadPoint &point = points.at(i);
        if (!point.present)
            continue;
        row(m_rows, { titles.at(i), perMinute(point.requestsPerMinute), Look::number(point.overloaded),
                      perMinute(point.queueAvg), Look::number(qRound64(point.queueMax)),
                      milliseconds(point.waitAvgMs), milliseconds(point.workAvgMs),
                      percent(point.cpuProcess), percent(point.cpuSystem),
                      std::isnan(point.memoryMb) ? QStringLiteral("—")
                                                 : Look::bytes(qint64(point.memoryMb * 1024 * 1024)),
                      perMinute(point.connectionsAvg), perMinute(point.writesPerCommit) });
    }

    const QString empty = QStringLiteral("За этот период сервер не работал или не вёл замеров");
    for (Chart *chart : { m_requestChart, m_queueChart, m_timeChart, m_cpuChart, m_memoryChart,
                          m_connectionChart }) {
        chart->setEmptyText(empty);
    }
    m_queueChart->setEmptyText(QStringLiteral("Очереди не было: запросы сразу попадали к потокам"));
    m_requestChart->setData(ticks, titles, { requests, refused });
    m_queueChart->setData(ticks, titles, { queueAvg, queueMax });
    m_timeChart->setData(ticks, titles, { wait, work });
    m_cpuChart->setData(ticks, titles, { cpuServer, cpuSystem });
    m_memoryChart->setData(ticks, titles, { memory });
    m_connectionChart->setData(ticks, titles, { connections, busy });
}

void LoadPage::showThreads(const LoadNow &now)
{
    QStringList ticks;
    QStringList titles;
    Chart::Series done { QStringLiteral("запросов"), Chart::seriesColor(0), {} };
    m_threadRows->clear();
    qint64 total = 0;
    qint64 least = -1;
    qint64 most = 0;
    for (const qint64 count : now.doneByThread) {
        total += count;
        least = least < 0 ? count : qMin(least, count);
        most = qMax(most, count);
    }
    for (int i = 0; i < now.doneByThread.size(); ++i) {
        const qint64 count = now.doneByThread.at(i);
        ticks << QString::number(i + 1);
        titles << QStringLiteral("Поток %1").arg(i + 1);
        done.values << double(count);
        row(m_threadRows, { QStringLiteral("Поток %1").arg(i + 1), Look::number(count),
                            total > 0 ? percent(100.0 * double(count) / double(total)) : QStringLiteral("—"),
                            duration(now.busyMsByThread.value(i)) });
    }
    m_threadChart->setData(ticks, titles, { done });

    if (now.doneByThread.isEmpty()) {
        m_threadNote->setText(QStringLiteral("Сервер не сообщил о своих потоках."));
    } else if (total == 0) {
        m_threadNote->setText(QStringLiteral("Потоков %1; запросов с запуска ещё не было.")
                                  .arg(now.doneByThread.size()));
    } else {
        const double mean = double(total) / now.doneByThread.size();
        const bool shared = now.mode == QLatin1String("fair") || now.mode == QLatin1String("fifo");
        m_threadNote->setText(
            QStringLiteral("Запросы с запуска сервера: %1. От %2 до %3 на поток — отклонение от "
                           "среднего до %4.")
                .arg(shared ? QStringLiteral("каждый поток берёт следующий из общей очереди, как "
                                             "только освободится")
                            : QStringLiteral("у каждого потока своя очередь"),
                     Look::number(least), Look::number(most),
                     percent(100.0 * qMax(mean - double(least), double(most) - mean) / mean)));
    }
}
