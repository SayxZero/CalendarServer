#include "serverpage.h"

#include "adminsession.h"
#include "look.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QString native(const QString &path)
{
    return path.isEmpty() ? QStringLiteral("—") : QDir::toNativeSeparators(path);
}

} // namespace

ServerPage::ServerPage(AdminSession *session, QWidget *parent)
    : Page(QStringLiteral("Сервер"), session, true, parent)
{
    setSubtitle(QStringLiteral("Служба сервера синхронизации, адреса для планеров и файлы сервера."));

    // ---- state -------------------------------------------------------------
    QFrame *state = Look::card();
    auto *stateLayout = new QVBoxLayout(state);
    stateLayout->setContentsMargins(20, 18, 20, 18);
    stateLayout->setSpacing(6);
    m_state = new QLabel;
    m_state->setProperty("role", "status");
    m_state->setTextFormat(Qt::RichText);
    m_details = Look::caption(QString());
    m_ping = Look::caption(QString());
    m_note = new QLabel;
    m_note->setProperty("role", "problem");
    m_note->setWordWrap(true);
    m_note->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_note->hide();

    m_start = new QPushButton(QStringLiteral("Запустить"));
    m_start->setProperty("accent", true);
    m_stop = new QPushButton(QStringLiteral("Остановить"));
    m_restart = new QPushButton(QStringLiteral("Перезапустить"));
    connect(m_start, &QPushButton::clicked, this, [this] { control(true); });
    connect(m_stop, &QPushButton::clicked, this, [this] { control(false); });
    connect(m_restart, &QPushButton::clicked, this, [this] { control(true, true); });

    stateLayout->addWidget(m_state);
    stateLayout->addWidget(m_details);
    stateLayout->addWidget(m_ping);
    stateLayout->addSpacing(6);
    stateLayout->addWidget(m_note);
    stateLayout->addWidget(buttonRow({ m_start, m_stop, m_restart }));
    body()->addWidget(state);

    // ---- connection --------------------------------------------------------
    QFrame *connection = Look::card();
    auto *connectionLayout = new QVBoxLayout(connection);
    connectionLayout->setContentsMargins(20, 16, 20, 18);
    connectionLayout->setSpacing(10);
    connectionLayout->addWidget(Look::cardTitle(QStringLiteral("Подключение")));
    auto *connectionGrid = new QGridLayout;
    connectionGrid->setHorizontalSpacing(24);
    connectionGrid->setVerticalSpacing(8);
    connectionGrid->setColumnStretch(1, 1);
    m_port = Look::addRow(connectionGrid, QStringLiteral("Порт"));
    m_registration = Look::addRow(connectionGrid, QStringLiteral("Регистрация"));
    m_addresses = Look::addRow(connectionGrid, QStringLiteral("Адрес для планеров"));
    connectionLayout->addLayout(connectionGrid);
    connectionLayout->addWidget(Look::caption(
        QStringLiteral("Этот адрес вводят в планере: «Настройки → Аккаунт и синхронизация → "
                       "Адрес сервера». Порт и регистрация задаются в файле настроек и "
                       "вступают в силу после перезапуска сервера.")));
    body()->addWidget(connection);

    // ---- files -------------------------------------------------------------
    QFrame *files = Look::card();
    auto *filesLayout = new QVBoxLayout(files);
    filesLayout->setContentsMargins(20, 16, 20, 18);
    filesLayout->setSpacing(10);
    filesLayout->addWidget(Look::cardTitle(QStringLiteral("Файлы")));
    auto *filesGrid = new QGridLayout;
    filesGrid->setHorizontalSpacing(24);
    filesGrid->setVerticalSpacing(8);
    filesGrid->setColumnStretch(1, 1);
    m_database = Look::addRow(filesGrid, QStringLiteral("База данных"));
    m_dataDir = Look::addRow(filesGrid, QStringLiteral("Папка данных"));
    m_configFile = Look::addRow(filesGrid, QStringLiteral("Настройки"));
    m_logFile = Look::addRow(filesGrid, QStringLiteral("Журнал"));
    filesLayout->addLayout(filesGrid);
    m_editConfig = new QPushButton(QStringLiteral("Изменить настройки…"));
    connect(m_editConfig, &QPushButton::clicked, this, [this] {
        const QString file = this->session()->config().configFile;
#ifdef Q_OS_WIN
        // Notepad inherits the rights of the panel and can write the file in
        // the protected data folder; Explorer would not.
        QProcess::startDetached(QStringLiteral("notepad.exe"), { QDir::toNativeSeparators(file) });
#else
        QDesktopServices::openUrl(QUrl::fromLocalFile(file));
#endif
    });
    filesLayout->addWidget(buttonRow({ m_editConfig }));
    body()->addWidget(files);
    body()->addStretch(1);

    connect(session, &AdminSession::statusChanged, this, &ServerPage::showStatus);
    connect(session, &AdminSession::pingChanged, this, &ServerPage::showPing);
    showStatus();
    showPing();
    showSettings();
}

void ServerPage::activate()
{
    session()->reloadConfig();
    session()->refreshStatus();
    session()->refreshPing();
    showSettings();
}

void ServerPage::showStatus()
{
    const ServiceControl::Status &status = session()->status();
    const bool control = session()->viaService();
    QString headline;
    QColor color = Look::muted();
    QStringList details;
    QString note;

    if (!control) {
        // The panel works on a folder of its own: what it shows is the server
        // of that base, found by the lock it holds in the folder - not the
        // service, which before stood here as "Сервер работает" while the
        // base belonged to a console server on another port, or to none.
        showOwnFolderStatus();
        return;
    }

    switch (status.state) {
    case ServiceControl::State::Running:
        headline = QStringLiteral("Сервер работает");
        color = Look::good();
        break;
    case ServiceControl::State::Starting:
        headline = QStringLiteral("Сервер запускается…");
        color = Look::warning();
        break;
    case ServiceControl::State::Stopping:
        headline = QStringLiteral("Сервер останавливается…");
        color = Look::warning();
        break;
    case ServiceControl::State::Stopped:
        if (status.exitCode != 0) {
            headline = QStringLiteral("Сервер остановился с ошибкой (код %1)").arg(status.exitCode);
            color = Look::critical();
            note = recentProblems();
        } else {
            headline = QStringLiteral("Сервер остановлен");
        }
        break;
    case ServiceControl::State::Missing:
        headline = QStringLiteral("Служба не установлена");
        note = QStringLiteral("Панель работает с базой в папке %1. Служба появляется при "
                              "установке сервера; без неё сервер запускают вручную "
                              "(run-server.bat), и управлять им отсюда нельзя.")
                   .arg(native(session()->config().resolvedDataDir()));
        break;
    case ServiceControl::State::Unknown:
        headline = QStringLiteral("Состояние службы неизвестно");
        color = Look::warning();
        note = status.error;
        break;
    }

    if (status.state != ServiceControl::State::Missing) {
        details << QStringLiteral("Служба %1").arg(session()->service().name());
        details << (status.autoStart ? QStringLiteral("запускается вместе с системой")
                                     : QStringLiteral("запускается вручную"));
        if (status.state == ServiceControl::State::Running && status.pid)
            details << QStringLiteral("процесс %1").arg(status.pid);
        if (status.state == ServiceControl::State::Running && status.startedAt.isValid())
            details << QStringLiteral("работает %1").arg(Look::span(status.startedAt));
    }
    if (!control && status.state != ServiceControl::State::Missing) {
        note = QStringLiteral("Панель запущена с --data-dir или --config и работает с базой "
                              "в %1: служба показана только для сведения.")
                   .arg(native(session()->config().resolvedDataDir()));
    }

    m_state->setText(Look::dot(color, headline));
    m_details->setText(details.join(QStringLiteral(" · ")));
    m_details->setVisible(!details.isEmpty());
    m_note->setText(note);
    m_note->setVisible(!note.isEmpty());
    m_note->setProperty("role", status.state == ServiceControl::State::Missing || !control
                                    ? "caption"
                                    : "problem");
    m_note->style()->unpolish(m_note);
    m_note->style()->polish(m_note);

    const bool idle = status.state == ServiceControl::State::Stopped;
    const bool running = status.state == ServiceControl::State::Running;
    m_start->setVisible(!running && status.state != ServiceControl::State::Missing);
    m_start->setEnabled(control && idle);
    m_stop->setVisible(running);
    m_stop->setEnabled(control);
    m_restart->setVisible(running);
    m_restart->setEnabled(control);
}

void ServerPage::showOwnFolderStatus()
{
    const RuntimeInfo &runtime = session()->runtime();
    const QString folder = native(session()->config().resolvedDataDir());
    QString headline;
    QColor color = Look::muted();
    QStringList details;
    QString note;
    if (runtime.running) {
        headline = QStringLiteral("Сервер работает");
        color = Look::good();
        details << (runtime.service ? QStringLiteral("служба") : QStringLiteral("запущен вручную"));
        details << QStringLiteral("процесс %1").arg(runtime.pid);
        if (runtime.port)
            details << QStringLiteral("порт %1").arg(runtime.port);
        if (runtime.startedAt.isValid())
            details << QStringLiteral("работает %1").arg(Look::span(runtime.startedAt.toLocalTime()));
        note = runtime.service
                   ? QStringLiteral("С этой базой работает служба. Чтобы запускать и останавливать "
                                    "её отсюда, откройте панель без --data-dir и --config.")
                   : QStringLiteral("Панель работает с базой в %1. Этот сервер останавливают там "
                                    "же, где запустили, — в его окне (Ctrl+C).")
                         .arg(folder);
    } else {
        headline = QStringLiteral("Сервер этой базы не запущен");
        note = QStringLiteral("Панель работает с базой в %1. Сервер с ней запускают вручную: "
                              "run-server.bat -d \"%1\" или CalendarServer --data-dir. "
                              "Сервер версии до 1.2 о себе в папке не сообщает и здесь не виден.")
                   .arg(folder);
    }

    m_state->setText(Look::dot(color, headline));
    m_details->setText(details.join(QStringLiteral(" · ")));
    m_details->setVisible(!details.isEmpty());
    m_note->setText(note);
    m_note->setVisible(true);
    m_note->setProperty("role", "caption");
    m_note->style()->unpolish(m_note);
    m_note->style()->polish(m_note);
    m_start->hide();
    m_stop->hide();
    m_restart->hide();
    showSettings();
}

void ServerPage::showPing()
{
    const PingAnswer &ping = session()->ping();
    if (ping.answered) {
        m_ping->setText(QStringLiteral("Отвечает на запросы за %1 мс · версия %2 · API %3")
                            .arg(ping.milliseconds)
                            .arg(ping.version)
                            .arg(ping.api));
    } else if (!ping.at.isValid()) {
        m_ping->setText(QStringLiteral("Проверяю, отвечает ли сервер…"));
    } else {
        m_ping->setText(QStringLiteral("Не отвечает на порту %1%2")
                            .arg(session()->serverPort())
                            .arg(ping.error.isEmpty() ? QString() : QStringLiteral(": ") + ping.error));
    }
}

void ServerPage::showSettings()
{
    const ServerConfig &config = session()->config();
    const bool any = config.host == QHostAddress::Any || config.host == QHostAddress::AnyIPv4
                     || config.host == QHostAddress::AnyIPv6;
    // The port the server really listens on: given with -p, it is not in the
    // settings at all.
    const quint16 port = session()->serverPort();
    QString portText = any ? QStringLiteral("%1, на всех адресах машины").arg(port)
                           : QStringLiteral("%1, только на %2").arg(port).arg(config.host.toString());
    if (port != config.port)
        portText += QStringLiteral(" (так запущен сервер; в настройках — %1)").arg(config.port);
    m_port->setText(portText);
    m_registration->setText(config.allowRegistration
                                ? QStringLiteral("открыта: новый аккаунт может создать любой, "
                                                 "кто достучится до сервера")
                                : QStringLiteral("закрыта: входят только существующие аккаунты"));
    const QStringList addresses = config.clientAddresses(port);
    m_addresses->setText(addresses.isEmpty() ? QStringLiteral("у машины нет сетевых адресов")
                                             : addresses.join(QLatin1Char('\n')));

    // What is not yet written into the file waits in its WAL beside it.
    const QFileInfo database(session()->databasePath());
    const qint64 size = database.size() + QFileInfo(database.absoluteFilePath() + QStringLiteral("-wal")).size();
    m_database->setText(database.exists()
                            ? QStringLiteral("%1 (%2)").arg(native(database.absoluteFilePath()),
                                                             Look::bytes(size))
                            : QStringLiteral("%1 — ещё не создана").arg(native(database.absoluteFilePath())));
    m_dataDir->setText(native(config.resolvedDataDir()));
    m_configFile->setText(config.configFile.isEmpty()
                              ? QStringLiteral("нет: сервер работает с настройками по умолчанию")
                              : native(config.configFile));
    const QString log = session()->logPath();
    m_logFile->setText(log.isEmpty() ? QStringLiteral("не ведётся: сервер пишет журнал только в консоль")
                                     : native(log));
    m_editConfig->setEnabled(!config.configFile.isEmpty());
}

void ServerPage::control(bool start, bool restart)
{
    ServiceControl &service = session()->service();
    QString error;
    bool ok = true;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    if (!start || restart) {
        ok = service.stop(&error);
        if (ok && service.settle(30000) != ServiceControl::State::Stopped) {
            ok = false;
            error = QStringLiteral("Служба не остановилась за 30 секунд.");
        }
    }
    if (ok && start) {
        ok = service.start(&error);
        if (ok) {
            const ServiceControl::State state = service.settle(30000);
            if (state != ServiceControl::State::Running) {
                ok = false;
                const QString problems = recentProblems();
                error = state == ServiceControl::State::Starting
                            ? QStringLiteral("Служба запускается дольше 30 секунд.")
                            : QStringLiteral("Сервер не запустился.%1")
                                  .arg(problems.isEmpty() ? QString() : QStringLiteral("\n\n") + problems);
            }
        }
    }
    QApplication::restoreOverrideCursor();
    session()->refreshStatus();
    if (!ok) {
        QMessageBox::warning(this, start ? QStringLiteral("Запуск сервера")
                                         : QStringLiteral("Остановка сервера"),
                             error);
    }
}

QString ServerPage::recentProblems() const
{
    QFile log(session()->logPath());
    if (!log.open(QIODevice::ReadOnly))
        return QString();
    // The end of the file is enough: the server writes why it stopped last.
    if (log.size() > 64 * 1024)
        log.seek(log.size() - 64 * 1024);
    const QStringList lines = QString::fromUtf8(log.readAll()).split(QLatin1Char('\n'));
    QStringList problems;
    for (const QString &line : lines) {
        if (line.contains(QLatin1String("[!!]")))
            problems << line.trimmed();
    }
    return problems.mid(qMax(0, int(problems.size()) - 3)).join(QLatin1Char('\n'));
}
