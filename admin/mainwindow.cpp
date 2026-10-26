#include "mainwindow.h"

#include "adminsession.h"
#include "backuppage.h"
#include "logpage.h"
#include "look.h"
#include "serverpage.h"
#include "statspage.h"
#include "userspage.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

MainWindow::MainWindow(AdminSession *session, QWidget *parent)
    : QMainWindow(parent)
    , m_session(session)
{
    setWindowTitle(QStringLiteral("Админ-панель сервера — Космос календарь"));

    auto *central = new QWidget;
    auto *layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---- the list of pages ---------------------------------------------------
    auto *sidebar = new QWidget;
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setAttribute(Qt::WA_StyledBackground);
    sidebar->setFixedWidth(210);
    auto *side = new QVBoxLayout(sidebar);
    side->setContentsMargins(0, 20, 0, 16);
    side->setSpacing(4);
    auto *brand = new QLabel(QStringLiteral("Космос календарь"));
    brand->setProperty("role", "brand");
    auto *what = new QLabel(QStringLiteral("Админ-панель сервера"));
    what->setProperty("role", "caption");
    auto *head = new QVBoxLayout;
    head->setContentsMargins(20, 0, 20, 12);
    head->setSpacing(0);
    head->addWidget(brand);
    head->addWidget(what);
    side->addLayout(head);

    m_nav = new QListWidget;
    m_nav->setObjectName(QStringLiteral("nav"));
    m_nav->setFrameShape(QFrame::NoFrame);
    m_nav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    side->addWidget(m_nav, 1);

    m_status = new QLabel;
    m_status->setTextFormat(Qt::RichText);
    m_answer = new QLabel;
    m_answer->setProperty("role", "hint");
    m_answer->setWordWrap(true);
    auto *foot = new QVBoxLayout;
    foot->setContentsMargins(20, 0, 20, 0);
    foot->setSpacing(2);
    foot->addWidget(m_status);
    foot->addWidget(m_answer);
    side->addLayout(foot);
    layout->addWidget(sidebar);

    // ---- the pages -----------------------------------------------------------
    m_stack = new QStackedWidget;
    m_users = new UsersPage(session);
    m_stats = new StatsPage(session);
    m_pages = { new ServerPage(session), m_users, m_stats, new BackupPage(session),
                new LogPage(session) };
    for (Page *page : m_pages) {
        m_stack->addWidget(page);
        m_nav->addItem(page->title());
    }
    layout->addWidget(m_stack, 1);
    setCentralWidget(central);

    statusBar()->showMessage(
        QStringLiteral("База: %1%2")
            .arg(QDir::toNativeSeparators(session->databasePath()),
                 session->viaService() ? QString() : QStringLiteral(" · папка задана при запуске панели")));

    connect(m_nav, &QListWidget::currentRowChanged, this, &MainWindow::showPage);
    connect(session, &AdminSession::statusChanged, this, &MainWindow::showStatus);
    connect(session, &AdminSession::pingChanged, this, &MainWindow::showStatus);
    m_nav->setCurrentRow(0);
    showStatus();

    QSettings settings;
    if (!restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray()))
        resize(1280, 820);
}

void MainWindow::showPage(int index)
{
    if (index < 0 || index >= m_pages.size())
        return;
    m_stack->setCurrentIndex(index);
    m_pages.at(index)->activate();
}

void MainWindow::showStatus()
{
    const ServiceControl::Status &status = m_session->status();
    QColor color = Look::muted();
    QString text;
    // A panel opened on a folder speaks of the server of that base.
    const RuntimeInfo &runtime = m_session->runtime();
    if (!m_session->viaService()) {
        color = runtime.running ? Look::good() : Look::muted();
        text = runtime.running ? QStringLiteral("Сервер работает")
                               : QStringLiteral("Сервер не запущен");
    } else switch (status.state) {
    case ServiceControl::State::Running:
        color = Look::good();
        text = QStringLiteral("Сервер работает");
        break;
    case ServiceControl::State::Starting:
    case ServiceControl::State::Stopping:
        color = Look::warning();
        text = QStringLiteral("Сервер %1").arg(ServiceControl::stateText(status.state));
        break;
    case ServiceControl::State::Stopped:
        color = status.exitCode ? Look::critical() : Look::muted();
        text = status.exitCode ? QStringLiteral("Сервер остановился с ошибкой")
                               : QStringLiteral("Сервер остановлен");
        break;
    case ServiceControl::State::Missing:
        text = QStringLiteral("Служба не установлена");
        break;
    case ServiceControl::State::Unknown:
        color = Look::warning();
        text = QStringLiteral("Состояние службы неизвестно");
        break;
    }
    m_status->setText(Look::dot(color, text));
    const PingAnswer &ping = m_session->ping();
    const quint16 port = m_session->serverPort();
    m_answer->setText(ping.answered       ? QStringLiteral("отвечает на порту %1").arg(port)
                      : ping.at.isValid() ? QStringLiteral("на порту %1 не отвечает").arg(port)
                                          : QStringLiteral("проверяю порт %1…").arg(port));
}

void MainWindow::grabPages(const QString &folder)
{
    QDir().mkpath(folder);
    const auto save = [&](const QString &name) {
        QApplication::processEvents();
        grab().save(QDir(folder).absoluteFilePath(name + QStringLiteral(".png")));
        // A page that scrolls, whole: what the window shows is only its top.
        if (QWidget *content = m_pages.at(m_stack->currentIndex())->scrolledContent())
            content->grab().save(QDir(folder).absoluteFilePath(name + QStringLiteral("-whole.png")));
    };
    const QStringList names = { QStringLiteral("server"), QStringLiteral("users"),
                                QStringLiteral("stats"), QStringLiteral("backups"),
                                QStringLiteral("log") };
    for (int i = 0; i < m_pages.size(); ++i) {
        m_nav->setCurrentRow(i);
        if (m_pages.at(i) == m_users) {
            for (int tab = 0; tab < m_users->tabCount(); ++tab) {
                m_users->showFirstAccount(tab);
                save(QStringLiteral("%1-users-%2").arg(i + 1).arg(m_users->tabName(tab)));
            }
            m_users->grabDialogs(folder);
            continue;
        }
        if (m_pages.at(i) == m_stats) {
            m_stats->showTables(false);
            save(QStringLiteral("%1-stats").arg(i + 1));
            m_stats->showTables(true);
            save(QStringLiteral("%1-stats-tables").arg(i + 1));
            m_stats->showTables(false);
            continue;
        }
        save(QStringLiteral("%1-%2").arg(i + 1).arg(names.value(i)));
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    QMainWindow::closeEvent(event);
}
