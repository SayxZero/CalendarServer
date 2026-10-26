#include "backuppage.h"

#include "adminsession.h"
#include "look.h"
#include "runtimefile.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

const int kPathRole = Qt::UserRole + 1;

QString native(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

} // namespace

BackupPage::BackupPage(AdminSession *session, QWidget *parent)
    : Page(QStringLiteral("Резервные копии"), session, true, parent)
{
    setSubtitle(QStringLiteral("Копия снимается на ходу: сервер для неё останавливать не нужно."));

    QFrame *card = Look::card();
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 16, 20, 18);
    layout->setSpacing(10);
    layout->addWidget(Look::cardTitle(QStringLiteral("Копии базы")));
    m_folder = Look::caption(QString());
    m_folder->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_folder);

    auto *create = new QPushButton(QStringLiteral("Создать копию"));
    create->setProperty("accent", true);
    auto *folder = new QPushButton(QStringLiteral("Другая папка…"));
    connect(create, &QPushButton::clicked, this, &BackupPage::create);
    connect(folder, &QPushButton::clicked, this, &BackupPage::chooseFolder);
    layout->addWidget(buttonRow({ create, folder }));

    m_list = new QTreeWidget;
    m_list->setHeaderLabels({ QStringLiteral("Файл"), QStringLiteral("Создана"), QStringLiteral("Размер"),
                              QStringLiteral("Аккаунтов"), QStringLiteral("Записей") });
    m_list->setRootIsDecorated(false);
    m_list->setAlternatingRowColors(true);
    m_list->setUniformRowHeights(true);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 5; ++column)
        m_list->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    m_list->setMinimumHeight(260);
    layout->addWidget(m_list, 1);
    m_summary = Look::caption(QString());
    m_summary->setProperty("role", "hint");
    layout->addWidget(m_summary);

    m_restore = new QPushButton(QStringLiteral("Восстановить из выбранной…"));
    auto *fromFile = new QPushButton(QStringLiteral("Восстановить из файла…"));
    m_export = new QPushButton(QStringLiteral("Сохранить копию как…"));
    connect(m_restore, &QPushButton::clicked, this, &BackupPage::restoreSelected);
    connect(fromFile, &QPushButton::clicked, this, &BackupPage::restoreFromFile);
    connect(m_export, &QPushButton::clicked, this, &BackupPage::exportSelected);
    layout->addWidget(buttonRow({ m_restore, fromFile, m_export }));
    body()->addWidget(card);

    QFrame *about = Look::card();
    auto *aboutLayout = new QVBoxLayout(about);
    aboutLayout->setContentsMargins(20, 16, 20, 18);
    aboutLayout->setSpacing(8);
    aboutLayout->addWidget(Look::cardTitle(QStringLiteral("Как работает восстановление")));
    aboutLayout->addWidget(Look::caption(
        QStringLiteral("Панель останавливает сервер, откладывает текущую базу в папку копий "
                       "(файл before-restore-…), ставит на её место выбранную копию и запускает "
                       "сервер снова. Аккаунты, созданные после копии, и сеансы устройств, вошедших "
                       "позже, пропадут.")));
    aboutLayout->addWidget(Look::caption(
        QStringLiteral("Устройства сами об откате не узнают: записи, изменённые после копии, "
                       "останутся у них в новом виде, а на сервере — в старом. Чтобы сервер "
                       "получил их обратно, на каждом устройстве в меню аккаунта планера есть "
                       "«Выгрузить всё заново…».")));
    body()->addWidget(about);
    body()->addStretch(1);

    connect(m_list, &QTreeWidget::currentItemChanged, this, [this] {
        const bool chosen = !selectedPath().isEmpty();
        m_restore->setEnabled(chosen);
        m_export->setEnabled(chosen);
    });
    reload();
}

void BackupPage::activate()
{
    reload();
}

QString BackupPage::selectedPath() const
{
    const QTreeWidgetItem *item = m_list->currentItem();
    return item ? item->data(0, kPathRole).toString() : QString();
}

void BackupPage::reload()
{
    const QString folder = session()->backupFolder();
    m_folder->setText(QStringLiteral("Папка копий: %1").arg(native(folder)));
    const QString keep = selectedPath();
    m_backups = Backups::list(folder);
    m_list->clear();
    qint64 total = 0;
    for (const BackupFile &backup : m_backups) {
        auto *item = new QTreeWidgetItem(m_list);
        item->setData(0, kPathRole, backup.path);
        item->setText(0, QFileInfo(backup.path).fileName());
        item->setText(1, Look::moment(backup.modified));
        item->setText(2, Look::bytes(backup.size));
        if (backup.info.ok) {
            item->setText(3, Look::number(backup.info.users));
            item->setText(4, Look::number(backup.info.records));
        } else {
            item->setText(3, QStringLiteral("не открывается"));
            item->setToolTip(3, backup.info.error);
            item->setForeground(3, Look::critical());
        }
        for (int column = 2; column < 5; ++column)
            item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
        if (backup.path == keep)
            m_list->setCurrentItem(item);
        total += backup.size;
    }
    m_summary->setText(m_backups.isEmpty()
                           ? QStringLiteral("Копий пока нет.")
                           : QStringLiteral("%1 %2, вместе %3.")
                                 .arg(Look::number(m_backups.size()))
                                 .arg(Look::plural(m_backups.size(), QStringLiteral("копия"),
                                                   QStringLiteral("копии"), QStringLiteral("копий")))
                                 .arg(Look::bytes(total)));
    const bool chosen = !selectedPath().isEmpty();
    m_restore->setEnabled(chosen);
    m_export->setEnabled(chosen);
}

void BackupPage::create()
{
    QString error;
    if (!session()->ensureDatabase(&error)) {
        QMessageBox::warning(this, QStringLiteral("Резервная копия"),
                             QStringLiteral("Копировать нечего: %1").arg(error));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString path;
    const bool ok = Backups::create(session()->databasePath(), session()->backupFolder(), &path, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Резервная копия"),
                             QStringLiteral("Копия не создана: %1").arg(error));
        return;
    }
    reload();
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        if (m_list->topLevelItem(i)->data(0, kPathRole).toString() == path)
            m_list->setCurrentItem(m_list->topLevelItem(i));
    }
}

void BackupPage::chooseFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Папка для копий"),
                                                             session()->backupFolder());
    if (folder.isEmpty())
        return;
    session()->setBackupFolder(folder);
    reload();
}

void BackupPage::exportSelected()
{
    const QString path = selectedPath();
    if (path.isEmpty())
        return;
    const QString target = QFileDialog::getSaveFileName(
        this, QStringLiteral("Сохранить копию как"),
        QDir::home().absoluteFilePath(QFileInfo(path).fileName()),
        QStringLiteral("База сервера (*.db)"));
    if (target.isEmpty())
        return;
    if (QFileInfo::exists(target))
        QFile::remove(target);
    if (!QFile::copy(path, target)) {
        QMessageBox::warning(this, QStringLiteral("Сохранить копию"),
                             QStringLiteral("Не удалось записать %1").arg(native(target)));
    }
}

void BackupPage::restoreSelected()
{
    const QString path = selectedPath();
    if (!path.isEmpty())
        restore(path);
}

void BackupPage::restoreFromFile()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Восстановить из файла"),
                                                      session()->backupFolder(),
                                                      QStringLiteral("База сервера (*.db);;Все файлы (*)"));
    if (!path.isEmpty())
        restore(path);
}

void BackupPage::restore(const QString &path)
{
    const QString title = QStringLiteral("Восстановление");
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const DatabaseInfo info = Storage::inspect(path, true);
    const DatabaseInfo current = Storage::inspect(session()->databasePath());
    QApplication::restoreOverrideCursor();
    if (!info.ok) {
        QMessageBox::warning(this, title, QStringLiteral("Из %1 восстановить нельзя: %2.")
                                              .arg(native(path), info.error));
        return;
    }

    const ServiceControl::Status status = session()->service().status();
    const bool control = session()->viaService();
    // The service matters only when it is the server of this base: opened on
    // a folder of its own, the panel used to refuse because the service was
    // running - with another base altogether.
    const bool running = control
                         && (status.state == ServiceControl::State::Running
                             || status.state == ServiceControl::State::Starting);
    // Whatever runs on this base holds server.lock in its folder, the service
    // or a console server on a port of its own; a server the panel cannot
    // stop would go on writing into the base being replaced.
    const QString folder = session()->config().resolvedDataDir();
    const RuntimeInfo runtime = RuntimeFile::read(folder);
    if (runtime.running && !(running && runtime.service)) {
        QMessageBox::warning(this, title,
                             QStringLiteral("С этой базой работает сервер, которым панель не "
                                            "управляет (процесс %1%2). Остановите его и повторите.")
                                 .arg(runtime.pid)
                                 .arg(runtime.port ? QStringLiteral(", порт %1").arg(runtime.port)
                                                   : QString()));
        return;
    }
    // An answer on the port of the settings is no longer asked: it came from
    // whatever listened there - the service with a base of its own, say - and
    // refused a restore of a base nobody used. The server ships with the panel
    // and holds the lock from version 1.2 on.

    QString question = QStringLiteral("Заменить базу сервера копией %1?\n\n"
                                      "В копии аккаунтов: %2, записей: %3, последняя активность: %4.")
                           .arg(QFileInfo(path).fileName())
                           .arg(info.users)
                           .arg(info.records)
                           .arg(Look::moment(info.lastActivity));
    if (current.ok) {
        question += QStringLiteral("\nСейчас в базе аккаунтов: %1, записей: %2.")
                        .arg(current.users)
                        .arg(current.records);
    }
    question += running ? QStringLiteral("\n\nСервер остановится на время замены и запустится снова.")
                        : QString();
    question += QStringLiteral("\nТекущая база будет отложена в папку копий.");
    // Buttons of its own: the standard ones would say "Yes" and "Cancel", the
    // translations of Qt do not come with the panel.
    QMessageBox ask(QMessageBox::Question, title, question, QMessageBox::NoButton, this);
    QPushButton *yes = ask.addButton(QStringLiteral("Восстановить"), QMessageBox::AcceptRole);
    ask.setDefaultButton(ask.addButton(QStringLiteral("Отмена"), QMessageBox::RejectRole));
    ask.exec();
    if (ask.clickedButton() != yes)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    ServiceControl &service = session()->service();
    QString error;
    if (running) {
        if (!service.stop(&error) || service.settle(30000) != ServiceControl::State::Stopped) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning(this, title,
                                 QStringLiteral("Сервер не остановился, база не тронута. %1").arg(error));
            session()->refreshStatus();
            return;
        }
    }
    // The base is the panel's while it is replaced: a server started meanwhile
    // - by hand, or the service by the system - finds the folder taken and
    // does not open the half-written file.
    RuntimeFile guard(folder);
    QString held;
    if (!guard.claim(&held)) {
        if (running)
            service.start(&error);
        QApplication::restoreOverrideCursor();
        session()->refreshStatus();
        QMessageBox::warning(this, title, QStringLiteral("База занята, она не тронута: %1.").arg(held));
        return;
    }
    session()->releaseDatabase();
    const RestoreOutcome outcome =
        Backups::restore(path, session()->databasePath(), session()->backupFolder());
    session()->reopenDatabase();
    guard.release();
    QString startError;
    bool started = true;
    if (running) {
        started = service.start(&startError)
                  && service.settle(30000) == ServiceControl::State::Running;
    }
    session()->refreshStatus();
    QApplication::restoreOverrideCursor();
    reload();

    if (!outcome.ok) {
        QMessageBox::warning(this, title, outcome.error);
        return;
    }
    QString done = QStringLiteral("База восстановлена из %1: аккаунтов %2, записей %3.")
                       .arg(QFileInfo(path).fileName())
                       .arg(outcome.users)
                       .arg(outcome.records);
    if (!outcome.safetyCopy.isEmpty())
        done += QStringLiteral("\nПрежняя база отложена: %1").arg(native(outcome.safetyCopy));
    if (!started)
        done += QStringLiteral("\n\nСервер не запустился: %1 Подробности — на странице «Журнал».")
                    .arg(startError);
    done += QStringLiteral("\n\nНа устройствах, где после копии что-то меняли, выполните в планере "
                           "«Выгрузить всё заново…» из меню аккаунта.");
    QMessageBox::information(this, title, done);
}
