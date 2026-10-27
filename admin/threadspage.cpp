#include "threadspage.h"

#include "adminsession.h"
#include "benchmarkrunner.h"
#include "inifile.h"
#include "loadrun.h"
#include "look.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

QString native(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

QString clock(double seconds)
{
    const int total = qMax(0, qRound(seconds));
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

QString number(double value)
{
    return QLocale(QLocale::Russian).toString(value, 'f', value < 10 ? 1 : 0);
}

/*! What the stages of the benchmark are called in the table. */
QString stageTitle(const QString &id)
{
    if (id == QLatin1String("threads"))
        return QStringLiteral("Потоки");
    if (id == QLatin1String("commit"))
        return QStringLiteral("Запись");
    if (id == QLatin1String("queue"))
        return QStringLiteral("Очередь");
    if (id == QLatin1String("sync"))
        return QStringLiteral("Без fsync");
    if (id == QLatin1String("memory"))
        return QStringLiteral("Память");
    if (id == QLatin1String("final"))
        return QStringLiteral("Итог");
    return id;
}

ServerSettings settingsOf(const ServerConfig &config)
{
    ServerSettings settings;
    settings.threads = config.threads;
    settings.dispatch = config.dispatch;
    settings.groupCommit = config.groupCommit;
    settings.syncMode = config.syncMode;
    settings.maxConnections = config.maxConnections;
    settings.bufferMb = config.bufferMb;
    settings.maxQueueWaitSeconds = config.maxQueueWaitSeconds;
    settings.maxBodyMb = config.maxBodyMb;
    return settings;
}

} // namespace

ThreadsPage::ThreadsPage(AdminSession *session, QWidget *parent)
    : Page(QStringLiteral("Многопоточность"), session, true, parent)
    , m_runner(new BenchmarkRunner(this))
    , m_tick(new QTimer(this))
{
    setSubtitle(QStringLiteral("Потоки сервера, очередь к ним, запись в базу и память — и бенчмарк, "
                               "который подбирает их для этой машины. Менять их и запускать бенчмарк "
                               "можно, пока сервер остановлен."));

    // ---- the state of the server ---------------------------------------------
    QFrame *stateCard = Look::card();
    auto *stateLayout = new QHBoxLayout(stateCard);
    stateLayout->setContentsMargins(20, 14, 20, 14);
    m_state = new QLabel;
    m_state->setTextFormat(Qt::RichText);
    m_state->setWordWrap(true);
    stateLayout->addWidget(m_state, 1);
    m_stopServer = new QPushButton(QStringLiteral("Остановить сервер"));
    stateLayout->addWidget(m_stopServer);
    body()->addWidget(stateCard);

    // ---- the settings --------------------------------------------------------
    QFrame *settings = Look::card();
    auto *settingsLayout = new QVBoxLayout(settings);
    settingsLayout->setContentsMargins(20, 16, 20, 16);
    settingsLayout->setSpacing(10);
    settingsLayout->addWidget(Look::cardTitle(QStringLiteral("Настройки")));
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(10);
    grid->setColumnStretch(2, 1);
    int row = 0;
    const auto field = [&](const QString &label, QWidget *widget, const QString &hint) {
        auto *name = new QLabel(label);
        name->setProperty("role", "rowLabel");
        QLabel *note = Look::caption(hint);
        grid->addWidget(name, row, 0, Qt::AlignTop);
        grid->addWidget(widget, row, 1, Qt::AlignTop);
        grid->addWidget(note, row, 2);
        widget->setMinimumWidth(260);
        m_fields << widget;
        ++row;
    };
    const int cores = qMax(2, QThread::idealThreadCount());
    m_threads = new QSpinBox;
    m_threads->setRange(0, 256);
    m_threads->setSpecialValueText(QStringLiteral("авто — по ядрам (%1)").arg(cores));
    field(QStringLiteral("Потоков"), m_threads,
          QStringLiteral("Сколько запросов сервер обрабатывает одновременно. Больше ядер имеет смысл, "
                         "когда запросы ждут диск."));
    m_dispatch = new QComboBox;
    m_dispatch->addItem(QStringLiteral("Справедливая — клиенты по очереди"), QStringLiteral("fair"));
    m_dispatch->addItem(QStringLiteral("По порядку прихода"), QStringLiteral("fifo"));
    m_dispatch->addItem(QStringLiteral("Своя у потока, по кругу"), QStringLiteral("round_robin"));
    m_dispatch->addItem(QStringLiteral("Своя у потока, в самую короткую"), QStringLiteral("least_loaded"));
    field(QStringLiteral("Очередь к потокам"), m_dispatch,
          QStringLiteral("Справедливая не даёт одному шумному клиенту задержать всех остальных; другие — "
                         "для сравнения."));
    m_groupCommit = new QSpinBox;
    m_groupCommit->setRange(0, 4096);
    m_groupCommit->setSpecialValueText(QStringLiteral("выключена"));
    field(QStringLiteral("Групповая запись"), m_groupCommit,
          QStringLiteral("Сколько запросов уходит на диск одним сбросом. На жёстком диске от неё зависит, "
                         "сколько правок в секунду сервер принимает."));
    m_syncMode = new QComboBox;
    m_syncMode->addItem(QStringLiteral("С fsync каждого сброса"), QStringLiteral("full"));
    m_syncMode->addItem(QStringLiteral("Без fsync"), QStringLiteral("normal"));
    field(QStringLiteral("Сброс на диск"), m_syncMode,
          QStringLiteral("Без fsync запись быстрее, но при отключении питания пропадут обмены последних "
                         "секунд, а устройства будут считать их принятыми. Только с ИБП."));
    m_maxConnections = new QSpinBox;
    m_maxConnections->setRange(16, 100000);
    field(QStringLiteral("Соединений не больше"), m_maxConnections,
          QStringLiteral("Остальные ждут в очереди системы, пока место не освободится."));
    m_bufferMb = new QSpinBox;
    m_bufferMb->setRange(0, 65536);
    m_bufferMb->setSuffix(QStringLiteral(" МБ"));
    m_bufferMb->setSpecialValueText(QStringLiteral("авто — восьмая часть памяти"));
    field(QStringLiteral("Память под запросы"), m_bufferMb,
          QStringLiteral("Тела запросов и их разбор сразу; сверх этого следующие запросы ждут."));
    m_queueWait = new QSpinBox;
    m_queueWait->setRange(1, 3600);
    m_queueWait->setSuffix(QStringLiteral(" с"));
    field(QStringLiteral("Ожидание в очереди"), m_queueWait,
          QStringLiteral("Запрос, прождавший поток дольше, получает «сервер перегружен» и повторяется "
                         "клиентом позже."));
    settingsLayout->addLayout(grid);
    m_file = Look::caption(QString());
    settingsLayout->addWidget(m_file);
    m_save = new QPushButton(QStringLiteral("Сохранить"));
    m_revert = new QPushButton(QStringLiteral("Как в файле"));
    m_defaults = new QPushButton(QStringLiteral("По умолчанию"));
    m_recommended = new QPushButton(QStringLiteral("Подставить рекомендованные"));
    settingsLayout->addWidget(buttonRow({ m_save, m_revert, m_defaults, m_recommended }));
    m_saved = Look::caption(QString());
    m_saved->setProperty("role", "hint");
    settingsLayout->addWidget(m_saved);
    body()->addWidget(settings);

    // ---- the benchmark -------------------------------------------------------
    QFrame *bench = Look::card();
    auto *benchLayout = new QVBoxLayout(bench);
    benchLayout->setContentsMargins(20, 16, 20, 16);
    benchLayout->setSpacing(8);
    benchLayout->addWidget(Look::cardTitle(QStringLiteral("Бенчмарк")));
    benchLayout->addWidget(Look::caption(
        QStringLiteral("Проверяет процессор, память и диск, затем запускает сервер на тестовой базе из "
                       "выдуманных аккаунтов — рядом с настоящей, на том же диске, но не трогая её, — и "
                       "подбирает число потоков, групповую запись и очередь. Настоящая база не "
                       "меняется; найденные настройки подставляются по кнопке и сохраняются вами.")));
    auto *controls = new QHBoxLayout;
    m_mode = new QComboBox;
    m_mode->addItem(QStringLiteral("Быстрый — около 4 минут"), false);
    m_mode->addItem(QStringLiteral("Подробный — около 12 минут"), true);
    m_start = new QPushButton(QStringLiteral("Запустить"));
    m_stop = new QPushButton(QStringLiteral("Остановить"));
    controls->addWidget(m_mode);
    controls->addWidget(m_start);
    controls->addWidget(m_stop);
    controls->addStretch(1);
    benchLayout->addLayout(controls);
    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);
    benchLayout->addWidget(m_progress);
    m_stage = new QLabel;
    m_stage->setProperty("role", "rowLabel");
    m_step = Look::caption(QString());
    m_alive = new QLabel;
    m_alive->setTextFormat(Qt::RichText);
    benchLayout->addWidget(m_stage);
    benchLayout->addWidget(m_step);
    benchLayout->addWidget(m_alive);
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMinimumHeight(170);
    m_log->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    benchLayout->addWidget(m_log);
    body()->addWidget(bench);

    // ---- the last result ---------------------------------------------------
    QFrame *result = Look::card();
    auto *resultLayout = new QVBoxLayout(result);
    resultLayout->setContentsMargins(20, 16, 20, 16);
    resultLayout->setSpacing(8);
    m_resultTitle = Look::cardTitle(QStringLiteral("Последний результат"));
    resultLayout->addWidget(m_resultTitle);
    m_system = Look::caption(QString());
    m_system->setTextInteractionFlags(Qt::TextSelectableByMouse);
    resultLayout->addWidget(m_system);
    m_advice = Look::caption(QString());
    m_advice->setTextInteractionFlags(Qt::TextSelectableByMouse);
    resultLayout->addWidget(m_advice);
    m_runs = new QTreeWidget;
    m_runs->setHeaderLabels({ QStringLiteral("Этап"), QStringLiteral("Прогон"), QStringLiteral("Запр./с"),
                              QStringLiteral("p50, мс"), QStringLiteral("p95, мс"), QStringLiteral("Отказы"),
                              QStringLiteral("Ошибки"), QStringLiteral("Пик памяти"),
                              QStringLiteral("За сброс") });
    m_runs->setRootIsDecorated(false);
    m_runs->setAlternatingRowColors(true);
    m_runs->setSelectionMode(QAbstractItemView::NoSelection);
    m_runs->setFocusPolicy(Qt::NoFocus);
    m_runs->header()->setStretchLastSection(false);
    m_runs->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int column : { 0, 2, 3, 4, 5, 6, 7, 8 })
        m_runs->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    m_runs->setMinimumHeight(260);
    resultLayout->addWidget(m_runs);
    body()->addWidget(result);
    body()->addStretch(1);

    // ---- wiring --------------------------------------------------------------
    connect(m_save, &QPushButton::clicked, this, &ThreadsPage::saveSettings);
    connect(m_revert, &QPushButton::clicked, this, &ThreadsPage::loadSettings);
    connect(m_defaults, &QPushButton::clicked, this, [this] {
        ServerSettings defaults;
        defaults.maxBodyMb = formSettings().maxBodyMb;
        showSettings(defaults);
        m_saved->setText(QStringLiteral("Подставлены значения по умолчанию — сохраните их."));
    });
    connect(m_recommended, &QPushButton::clicked, this, &ThreadsPage::applyRecommendation);
    connect(m_start, &QPushButton::clicked, this, &ThreadsPage::startBenchmark);
    connect(m_stop, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, QStringLiteral("Бенчмарк"),
                                  QStringLiteral("Остановить бенчмарк? Результат этого запуска пропадёт."),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            == QMessageBox::Yes) {
            m_runner->stop();
        }
    });
    connect(m_stopServer, &QPushButton::clicked, this, [this] {
        ServiceControl &service = this->session()->service();
        QString error;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        bool ok = service.stop(&error);
        if (ok && service.settle(30000) != ServiceControl::State::Stopped) {
            ok = false;
            error = QStringLiteral("Служба не остановилась за 30 секунд.");
        }
        QApplication::restoreOverrideCursor();
        this->session()->refreshStatus();
        if (!ok)
            QMessageBox::warning(this, QStringLiteral("Остановка сервера"), error);
    });
    connect(m_runner, &BenchmarkRunner::changed, this, &ThreadsPage::showProgress);
    connect(m_runner, &BenchmarkRunner::finished, this, [this] {
        m_tick->stop();
        this->session()->setBenchmarkRunning(false);
        loadLastResult();
        showState();
        showProgress();
    });
    connect(session, &AdminSession::statusChanged, this, &ThreadsPage::showState);
    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, &ThreadsPage::showProgress);

    loadSettings();
    loadLastResult();
    showState();
    showProgress();
}

ThreadsPage::~ThreadsPage()
{
    if (m_runner->isRunning())
        session()->setBenchmarkRunning(false);
}

void ThreadsPage::activate()
{
    if (!m_runner->isRunning()) {
        loadSettings();
        loadLastResult();
    }
    showState();
}

QString ThreadsPage::settingsPath() const
{
    const ServerConfig &config = session()->config();
    if (!config.configFile.isEmpty())
        return config.configFile;
    return QDir(config.resolvedDataDir()).filePath(QStringLiteral("server.ini"));
}

QString ThreadsPage::workFolder() const
{
    return QDir(session()->config().resolvedDataDir()).filePath(QStringLiteral("benchmark"));
}

bool ThreadsPage::serverStopped() const
{
    if (session()->runtime().running)
        return false;
    if (!session()->viaService())
        return true;
    const ServiceControl::State state = session()->status().state;
    return state == ServiceControl::State::Stopped || state == ServiceControl::State::Missing;
}

void ThreadsPage::loadSettings()
{
    const QString path = settingsPath();
    ServerConfig config;
    QString error;
    if (QFile::exists(path) && !config.loadIni(path, &error)) {
        m_saved->setText(QStringLiteral("В файле настроек ошибка: %1").arg(error));
    } else {
        m_saved->clear();
    }
    showSettings(settingsOf(config));
    m_file->setText(session()->config().configFile.isEmpty()
                        ? QStringLiteral("Файл: %1 — сервер этой папки читает его, если запущен с "
                                         "--config.")
                              .arg(native(path))
                        : QStringLiteral("Файл: %1%2")
                              .arg(native(path),
                                   QFile::exists(path) ? QString() : QStringLiteral(" (будет создан)")));
}

void ThreadsPage::showSettings(const ServerSettings &settings)
{
    m_threads->setValue(settings.threads);
    m_dispatch->setCurrentIndex(qMax(0, m_dispatch->findData(settings.dispatch)));
    m_groupCommit->setValue(settings.groupCommit);
    m_syncMode->setCurrentIndex(qMax(0, m_syncMode->findData(settings.syncMode)));
    m_maxConnections->setValue(settings.maxConnections);
    m_bufferMb->setValue(settings.bufferMb);
    m_queueWait->setValue(settings.maxQueueWaitSeconds);
    m_threads->setProperty("maxBodyMb", settings.maxBodyMb);
}

ServerSettings ThreadsPage::formSettings() const
{
    ServerSettings settings;
    settings.threads = m_threads->value();
    settings.dispatch = m_dispatch->currentData().toString();
    settings.groupCommit = m_groupCommit->value();
    settings.syncMode = m_syncMode->currentData().toString();
    settings.maxConnections = m_maxConnections->value();
    settings.bufferMb = m_bufferMb->value();
    settings.maxQueueWaitSeconds = m_queueWait->value();
    settings.maxBodyMb = m_threads->property("maxBodyMb").toInt();
    return settings;
}

void ThreadsPage::saveSettings()
{
    if (!serverStopped()) {
        QMessageBox::information(this, QStringLiteral("Настройки"),
                                 QStringLiteral("Сервер работает: остановите его, а потом меняйте настройки."));
        return;
    }
    const ServerSettings settings = formSettings();
    if (settings.syncMode == QLatin1String("normal")
        && QMessageBox::question(this, QStringLiteral("Сброс на диск без fsync"),
                                 QStringLiteral("Без fsync при отключении питания пропадут обмены последних "
                                                "секунд, а устройства будут считать их принятыми и не "
                                                "пришлют снова. Включать только при источнике "
                                                "бесперебойного питания. Сохранить так?"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
               != QMessageBox::Yes) {
        return;
    }
    const QList<QPair<QString, QString>> values = {
        { QStringLiteral("threads"), QString::number(settings.threads) },
        { QStringLiteral("dispatch"), settings.dispatch },
        { QStringLiteral("group_commit"), QString::number(settings.groupCommit) },
        { QStringLiteral("sync_mode"), settings.syncMode },
        { QStringLiteral("max_connections"), QString::number(settings.maxConnections) },
        { QStringLiteral("buffer_mb"), QString::number(settings.bufferMb) },
        { QStringLiteral("max_queue_wait_seconds"), QString::number(settings.maxQueueWaitSeconds) },
    };
    QString error;
    if (!IniFile::write(settingsPath(), QStringLiteral("server"), values, &error)) {
        QMessageBox::warning(this, QStringLiteral("Настройки"), error);
        return;
    }
    session()->reloadConfig();
    loadSettings();
    m_saved->setText(QStringLiteral("Сохранено в %1. Сервер возьмёт их при следующем запуске.")
                         .arg(QTime::currentTime().toString(QStringLiteral("HH:mm"))));
}

void ThreadsPage::applyRecommendation()
{
    if (m_recommendation.isEmpty())
        return;
    ServerSettings recommended =
        ServerSettings::fromJson(m_recommendation.value(QStringLiteral("settings")).toObject());
    // What the benchmark did not try stays as it is in the form.
    const ServerSettings form = formSettings();
    recommended.maxConnections = form.maxConnections;
    recommended.bufferMb = form.bufferMb;
    recommended.maxQueueWaitSeconds = form.maxQueueWaitSeconds;
    recommended.maxBodyMb = form.maxBodyMb;
    showSettings(recommended);
    m_saved->setText(QStringLiteral("Подставлены рекомендованные: %1 — сохраните их.")
                         .arg(recommended.describe()));
}

void ThreadsPage::startBenchmark()
{
    if (!serverStopped()) {
        QMessageBox::information(this, QStringLiteral("Бенчмарк"),
                                 QStringLiteral("Сервер работает: остановите его. Бенчмарк рядом с "
                                                "работающим сервером измерил бы их обоих и замедлил бы "
                                                "его клиентов."));
        return;
    }
    QStringList arguments = { QStringLiteral("tune"), QStringLiteral("--json"), QStringLiteral("--work"),
                              workFolder() };
    const QString config = settingsPath();
    if (QFile::exists(config))
        arguments << QStringLiteral("--config") << config;
    else
        arguments << QStringLiteral("--data-dir") << session()->config().resolvedDataDir();
    if (m_mode->currentData().toBool())
        arguments << QStringLiteral("--full");
    QString error;
    if (!m_runner->start(arguments, &error)) {
        QMessageBox::warning(this, QStringLiteral("Бенчмарк"), error);
        return;
    }
    session()->setBenchmarkRunning(true);
    m_tick->start();
    showState();
    showProgress();
}

void ThreadsPage::showState()
{
    const bool stopped = serverStopped();
    const bool benchmark = m_runner->isRunning();
    if (benchmark) {
        m_state->setText(Look::dot(Look::warning(), QStringLiteral("Идёт бенчмарк: сервер не запускайте, "
                                                                   "пока он не закончится")));
    } else if (!stopped) {
        m_state->setText(Look::dot(Look::good(), QStringLiteral("Сервер работает")) + QStringLiteral(
                             "<br>Настройки и бенчмарк доступны, когда сервер остановлен."));
    } else {
        m_state->setText(Look::dot(Look::muted(), QStringLiteral("Сервер остановлен"))
                         + QStringLiteral("<br>Можно менять настройки и запускать бенчмарк."));
    }
    const bool service = session()->viaService()
                         && session()->status().state == ServiceControl::State::Running;
    m_stopServer->setVisible(service && !benchmark);
    m_stopServer->setEnabled(session()->viaService());

    const bool editable = stopped && !benchmark;
    for (QWidget *widget : std::as_const(m_fields))
        widget->setEnabled(editable);
    m_save->setEnabled(editable);
    m_revert->setEnabled(editable);
    m_defaults->setEnabled(editable);
    m_recommended->setEnabled(editable && !m_recommendation.isEmpty());
    m_mode->setEnabled(editable);
    m_start->setEnabled(editable && QFile::exists(BenchmarkRunner::programPath()));
    m_start->setToolTip(QFile::exists(BenchmarkRunner::programPath())
                            ? QString()
                            : QStringLiteral("Нет %1").arg(native(BenchmarkRunner::programPath())));
    m_stop->setEnabled(benchmark);
}

void ThreadsPage::showProgress()
{
    const BenchmarkRunner::Outcome outcome = m_runner->outcome();
    m_progress->setValue(qRound(m_runner->percent() * 10));
    const auto stages = m_runner->stages();
    const int index = m_runner->stageIndex();
    if (outcome == BenchmarkRunner::Outcome::Idle) {
        m_stage->setText(QStringLiteral("Бенчмарк не запускался с открытия панели"));
        m_step->clear();
        m_alive->clear();
    } else {
        m_stage->setText(index >= 0 && index < stages.size()
                             ? QStringLiteral("Этап %1 из %2: %3")
                                   .arg(index + 1)
                                   .arg(stages.size())
                                   .arg(stages.at(index).second)
                             : QStringLiteral("Подготовка"));
        m_step->setText(m_runner->message());
    }

    const double elapsed = m_runner->elapsed();
    switch (outcome) {
    case BenchmarkRunner::Outcome::Idle:
        break;
    case BenchmarkRunner::Outcome::Running: {
        const double silence = m_runner->silence();
        const double idle = m_runner->idle();
        QString left;
        if (m_runner->percent() > 3) {
            left = QStringLiteral(", осталось около %1")
                       .arg(clock(elapsed * (100 - m_runner->percent()) / m_runner->percent()));
        }
        if (silence > 10) {
            m_alive->setText(Look::dot(Look::critical(),
                                       QStringLiteral("Бенчмарк не отвечает %1 с — его можно остановить")
                                           .arg(qRound(silence))));
        } else if (idle > 90) {
            m_alive->setText(Look::dot(Look::warning(),
                                       QStringLiteral("Нет продвижения %1 с — похоже, завис; его можно "
                                                      "остановить")
                                           .arg(qRound(idle))));
        } else {
            m_alive->setText(Look::dot(Look::good(),
                                       QStringLiteral("Работает: %1 %, прошло %2%3, отклик %4 с назад")
                                           .arg(qRound(m_runner->percent()))
                                           .arg(clock(elapsed), left)
                                           .arg(qRound(silence))));
        }
        break;
    }
    case BenchmarkRunner::Outcome::Done:
        m_alive->setText(Look::dot(Look::good(), QStringLiteral("Готово за %1").arg(clock(elapsed))));
        break;
    case BenchmarkRunner::Outcome::Failed:
        m_alive->setText(Look::dot(Look::critical(), QStringLiteral("Ошибка: %1").arg(m_runner->error())));
        break;
    case BenchmarkRunner::Outcome::Cancelled:
        m_alive->setText(Look::dot(Look::muted(), QStringLiteral("Остановлен через %1").arg(clock(elapsed))));
        break;
    }

    const QString text = m_runner->log().join(QLatin1Char('\n'));
    if (m_log->toPlainText() != text) {
        m_log->setPlainText(text);
        m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
    }
}

void ThreadsPage::loadLastResult()
{
    QFile file(QDir(workFolder()).filePath(QStringLiteral("result.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        m_resultTitle->setText(QStringLiteral("Последний результат"));
        m_system->setText(QStringLiteral("Бенчмарк на этой машине ещё не запускался."));
        m_advice->clear();
        m_runs->clear();
        m_recommendation = QJsonObject();
        showState();
        return;
    }
    showResult(QJsonDocument::fromJson(file.readAll()).object());
}

void ThreadsPage::showResult(const QJsonObject &document)
{
    const QDateTime finished =
        QDateTime::fromString(document.value(QStringLiteral("finishedAt")).toString(), Qt::ISODate);
    m_resultTitle->setText(QStringLiteral("Последний результат — %1, %2")
                               .arg(Look::moment(finished.toLocalTime()),
                                    document.value(QStringLiteral("mode")).toString()
                                            == QLatin1String("full")
                                        ? QStringLiteral("подробный")
                                        : QStringLiteral("быстрый")));
    const SystemInfo system = SystemInfo::fromJson(document.value(QStringLiteral("system")).toObject());
    m_system->setText(system.describe().join(QLatin1Char('\n')));

    m_recommendation = document.value(QStringLiteral("recommendation")).toObject();
    const ServerSettings recommended =
        ServerSettings::fromJson(m_recommendation.value(QStringLiteral("settings")).toObject());
    QStringList advice;
    advice << QStringLiteral("Рекомендовано: %1.").arg(recommended.describe());
    for (const QJsonValue &note : m_recommendation.value(QStringLiteral("notes")).toArray())
        advice << note.toString();
    m_advice->setText(advice.join(QLatin1Char('\n')));

    m_runs->clear();
    for (const QJsonValue &value : document.value(QStringLiteral("steps")).toArray()) {
        const QJsonObject step = value.toObject();
        const LoadResult result = LoadResult::fromJson(step.value(QStringLiteral("result")).toObject());
        auto *item = new QTreeWidgetItem(m_runs);
        item->setText(0, stageTitle(step.value(QStringLiteral("stage")).toString()));
        item->setText(1, step.value(QStringLiteral("label")).toString()
                             + (result.started ? QString() : QStringLiteral(" — ") + result.error));
        item->setText(2, result.started ? Look::number(qRound64(result.throughput)) : QStringLiteral("—"));
        item->setText(3, result.started ? number(result.p50) : QStringLiteral("—"));
        item->setText(4, result.started ? number(result.p95) : QStringLiteral("—"));
        item->setText(5, Look::number(result.refused));
        item->setText(6, result.died ? QStringLiteral("упал") : Look::number(result.errors));
        item->setText(7, result.peakMemoryMb > 0 ? QStringLiteral("%1 МБ").arg(qRound(result.peakMemoryMb))
                                                 : QStringLiteral("—"));
        item->setText(8, result.writesPerCommit > 0 ? number(result.writesPerCommit) : QStringLiteral("—"));
        for (int column = 2; column <= 8; ++column)
            item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
    }
    showState();
}
