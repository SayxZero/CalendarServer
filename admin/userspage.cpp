#include "userspage.h"

#include "adminsession.h"
#include "look.h"
#include "records.h"

#include "api.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

const int kSortRole = Qt::UserRole + 1;
const int kIdRole = Qt::UserRole + 2;

/*! A row that sorts by kSortRole when it has one: numbers and moments, not their text. */
class SortItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        const QVariant mine = data(column, kSortRole);
        const QVariant theirs = other.data(column, kSortRole);
        if (mine.isValid() && theirs.isValid())
            return mine.toDouble() < theirs.toDouble();
        return text(column).localeAwareCompare(other.text(column)) < 0;
    }
};

/*! A sortable list; @p stretch is the column that takes the room left over. */
QTreeWidget *makeTree(const QStringList &headers, int stretch = 0)
{
    auto *tree = new QTreeWidget;
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->setSortingEnabled(true);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->header()->setStretchLastSection(false);
    for (int i = 0; i < headers.size(); ++i) {
        tree->header()->setSectionResizeMode(i, i == stretch ? QHeaderView::Stretch
                                                             : QHeaderView::ResizeToContents);
    }
    return tree;
}

void setSort(QTreeWidgetItem *item, int column, double value)
{
    item->setData(column, kSortRole, value);
}

void setMoment(QTreeWidgetItem *item, int column, const QDateTime &moment)
{
    item->setText(column, Look::moment(moment));
    setSort(item, column, moment.isValid() ? double(moment.toMSecsSinceEpoch()) : 0.0);
}

QIcon colorDot(const QString &color)
{
    QPixmap pixmap(24, 24);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor fill(color);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill.isValid() ? fill : Look::grid());
    painter.drawEllipse(QRectF(1, 1, 10, 10));
    return QIcon(pixmap);
}

QString modeText(ShareMode mode)
{
    switch (mode) {
    case ShareMode::Read:
        return QStringLiteral("все на чтении");
    case ShareMode::Edit:
        return QStringLiteral("все правят");
    case ShareMode::Custom:
        return QStringLiteral("права у каждого свои");
    }
    return QString();
}

QString accessText(ShareAccess access)
{
    return access == ShareAccess::Edit ? QStringLiteral("правка") : QStringLiteral("чтение");
}

/*! The record whole: its stamps and the JSON the planner sent. */
class RecordDialog : public QDialog
{
public:
    RecordDialog(const StoredRecord &record, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("%1 %2").arg(PlannerRecord::kindName(record.kind), record.id));
        resize(640, 560);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(20, 18, 20, 16);
        layout->setSpacing(10);

        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(24);
        grid->setVerticalSpacing(6);
        grid->setColumnStretch(1, 1);
        Look::addRow(grid, QStringLiteral("Вид"),
                     QStringLiteral("%1 (%2)").arg(PlannerRecord::kindName(record.kind), record.kind));
        Look::addRow(grid, QStringLiteral("Идентификатор"), record.id);
        Look::addRow(grid, QStringLiteral("Изменена"),
                     Look::moment(QDateTime::fromMSecsSinceEpoch(record.modified)));
        Look::addRow(grid, QStringLiteral("Устройство"),
                     record.device.isEmpty() ? QStringLiteral("—") : record.device);
        Look::addRow(grid, QStringLiteral("Ревизия"), Look::number(record.revision));
        Look::addRow(grid, QStringLiteral("Размер"),
                     record.deleted ? QStringLiteral("удалена: хранится только надгробие")
                                    : Look::bytes(record.size));
        layout->addLayout(grid);

        auto *text = new QPlainTextEdit;
        text->setReadOnly(true);
        text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        text->setPlainText(record.deleted
                               ? QStringLiteral("Запись удалена. Сервер хранит её надгробие, чтобы "
                                                "об удалении узнали все устройства аккаунта.")
                               : QString::fromUtf8(QJsonDocument(record.data).toJson(QJsonDocument::Indented)));
        layout->addWidget(text, 1);

        auto *buttons = new QDialogButtonBox;
        buttons->addButton(QStringLiteral("Закрыть"), QDialogButtonBox::RejectRole);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
    }
};

/*! A new password, typed twice. */
class PasswordDialog : public QDialog
{
public:
    PasswordDialog(const UserSummary &user, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("Новый пароль"));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(20, 18, 20, 16);
        layout->setSpacing(10);
        layout->addWidget(Look::cardTitle(QStringLiteral("Новый пароль аккаунта «%1»").arg(user.login)));
        m_first = new QLineEdit;
        m_first->setEchoMode(QLineEdit::Password);
        m_first->setPlaceholderText(QStringLiteral("Новый пароль"));
        m_second = new QLineEdit;
        m_second->setEchoMode(QLineEdit::Password);
        m_second->setPlaceholderText(QStringLiteral("Ещё раз"));
        layout->addWidget(m_first);
        layout->addWidget(m_second);
        layout->addWidget(Look::caption(
            QStringLiteral("Не короче 6 символов. Все устройства, вошедшие в аккаунт (%1), выйдут "
                           "из него при следующем обмене, и войти снова можно будет только с "
                           "новым паролем. Записи аккаунта не меняются.")
                .arg(user.devices)));
        m_problem = new QLabel;
        m_problem->setProperty("role", "problem");
        m_problem->setWordWrap(true);
        m_problem->hide();
        layout->addWidget(m_problem);

        auto *buttons = new QDialogButtonBox;
        auto *apply = buttons->addButton(QStringLiteral("Сменить пароль"), QDialogButtonBox::AcceptRole);
        apply->setProperty("accent", true);
        buttons->addButton(QStringLiteral("Отмена"), QDialogButtonBox::RejectRole);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            QString problem = Api::passwordProblem(m_first->text());
            if (problem.isEmpty() && m_first->text() != m_second->text())
                problem = QStringLiteral("Пароли не совпадают");
            if (!problem.isEmpty()) {
                m_problem->setText(problem);
                m_problem->show();
                return;
            }
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        setMinimumWidth(420);
    }

    QString password() const { return m_first->text(); }

private:
    QLineEdit *m_first = nullptr;
    QLineEdit *m_second = nullptr;
    QLabel *m_problem = nullptr;
};

} // namespace

UsersPage::UsersPage(AdminSession *session, QWidget *parent)
    : Page(QStringLiteral("Пользователи"), session, false, parent)
{
    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);

    // ---- the list ----------------------------------------------------------
    QFrame *listCard = Look::card();
    auto *listLayout = new QVBoxLayout(listCard);
    listLayout->setContentsMargins(14, 14, 14, 14);
    listLayout->setSpacing(10);
    m_search = new QLineEdit;
    m_search->setPlaceholderText(QStringLiteral("Найти по логину или имени"));
    m_search->setClearButtonEnabled(true);
    m_users = makeTree({ QStringLiteral("Логин"), QStringLiteral("Имя"),
                         QStringLiteral("Последний обмен") },
                       1);
    m_users->sortByColumn(0, Qt::AscendingOrder);
    m_empty = Look::caption(QString());
    m_empty->setProperty("role", "hint");
    m_empty->hide();
    listLayout->addWidget(m_search);
    listLayout->addWidget(m_users, 1);
    listLayout->addWidget(m_empty);
    splitter->addWidget(listCard);

    // ---- the chosen account ------------------------------------------------
    m_details = Look::card();
    auto *details = new QVBoxLayout(m_details);
    details->setContentsMargins(20, 16, 20, 16);
    details->setSpacing(10);

    auto *headline = new QHBoxLayout;
    auto *names = new QVBoxLayout;
    names->setSpacing(0);
    m_login = new QLabel;
    m_login->setProperty("role", "status");
    m_login->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_name = Look::caption(QString());
    names->addWidget(m_login);
    names->addWidget(m_name);
    headline->addLayout(names, 1);
    m_password = new QPushButton(QStringLiteral("Сменить пароль…"));
    connect(m_password, &QPushButton::clicked, this, &UsersPage::changePassword);
    headline->addWidget(m_password, 0, Qt::AlignTop);
    details->addLayout(headline);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(1, 1);
    m_created = Look::addRow(grid, QStringLiteral("Создан"));
    m_lastSync = Look::addRow(grid, QStringLiteral("Последний обмен"));
    m_syncs = Look::addRow(grid, QStringLiteral("Обменов за 30 дней"));
    m_volume = Look::addRow(grid, QStringLiteral("Данные"));
    details->addLayout(grid);

    auto *filters = new QHBoxLayout;
    m_recordSearch = new QLineEdit;
    m_recordSearch->setPlaceholderText(QStringLiteral("Найти запись по названию"));
    m_recordSearch->setClearButtonEnabled(true);
    m_deleted = new QCheckBox(QStringLiteral("Удалённые записи"));
    filters->addWidget(m_recordSearch, 1);
    filters->addWidget(m_deleted);
    details->addLayout(filters);

    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    m_devices = makeTree({ QStringLiteral("Устройство"), QStringLiteral("Вошло"),
                           QStringLiteral("Последний раз") });
    m_calendars = makeTree({ QStringLiteral("Календарь"), QStringLiteral("Чей и кому открыт"),
                             QStringLiteral("Событий"), QStringLiteral("Задач"),
                             QStringLiteral("Прочего") },
                           1);
    m_calendars->setRootIsDecorated(true);
    m_events = makeTree({ QStringLiteral("Название"), QStringLiteral("Начало"),
                          QStringLiteral("Календарь"), QStringLiteral("Состояние"),
                          QStringLiteral("Изменено"), QStringLiteral("Устройство") });
    m_tasks = makeTree({ QStringLiteral("Название"), QStringLiteral("Срок"),
                         QStringLiteral("Календарь"), QStringLiteral("Состояние"),
                         QStringLiteral("Изменено"), QStringLiteral("Устройство") });
    m_other = makeTree({ QStringLiteral("Название"), QStringLiteral("Вид"), QStringLiteral("Когда"),
                         QStringLiteral("Изменено"), QStringLiteral("Устройство") });
    for (QTreeWidget *tree : { m_events, m_tasks, m_other })
        connect(tree, &QTreeWidget::itemActivated, this, &UsersPage::openRecord);
    m_tabs->addTab(m_calendars, QString());
    m_tabs->addTab(m_events, QString());
    m_tabs->addTab(m_tasks, QString());
    m_tabs->addTab(m_other, QString());
    m_tabs->addTab(m_devices, QString());
    details->addWidget(m_tabs, 1);
    details->addWidget(Look::caption(
        QStringLiteral("Двойной щелчок по записи открывает её целиком. Названия и даты панель "
                       "читает из записей планера; сами записи сервер хранит как есть.")));
    splitter->addWidget(m_details);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({ 380, 700 });
    body()->addWidget(splitter, 1);

    connect(m_search, &QLineEdit::textChanged, this, &UsersPage::reload);
    connect(m_users, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        m_current = item ? item->data(0, kIdRole).toLongLong() : 0;
        showAccount();
    });
    connect(m_recordSearch, &QLineEdit::textChanged, this, &UsersPage::fillRecords);
    connect(m_deleted, &QCheckBox::toggled, this, &UsersPage::fillRecords);
    connect(session, &AdminSession::databaseChanged, this, &UsersPage::reload);
    connect(session, &AdminSession::accountsChanged, this, &UsersPage::reload);
    reload();
}

void UsersPage::activate()
{
    reload();
}

int UsersPage::tabCount() const
{
    return m_tabs->count();
}

QString UsersPage::tabName(int index) const
{
    static const char *names[] = { "calendars", "events", "tasks", "other", "devices" };
    return QString::fromLatin1(names[qBound(0, index, 4)]);
}

void UsersPage::showFirstAccount(int tab)
{
    if (m_users->topLevelItemCount() > 0 && !m_users->currentItem())
        m_users->setCurrentItem(m_users->topLevelItem(0));
    m_tabs->setCurrentIndex(tab);
}

void UsersPage::grabDialogs(const QString &folder)
{
    showFirstAccount(1);
    const auto grab = [&folder](QDialog &dialog, const QString &name) {
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        QApplication::processEvents();
        dialog.grab().save(QDir(folder).absoluteFilePath(name + QStringLiteral(".png")));
        dialog.close();
    };
    for (const StoredRecord &record : m_records) {
        if (record.kind == QLatin1String("event") && !record.deleted) {
            RecordDialog dialog(record, this);
            grab(dialog, QStringLiteral("dialog-record"));
            break;
        }
    }
    for (const UserSummary &user : m_accounts) {
        if (user.id == m_current) {
            PasswordDialog dialog(user, this);
            grab(dialog, QStringLiteral("dialog-password"));
        }
    }
}

void UsersPage::reload()
{
    AdminDb &db = session()->db();
    QString error;
    if (!session()->ensureDatabase(&error)) {
        m_accounts.clear();
        m_users->clear();
        m_empty->setText(QStringLiteral("База не открыта: %1").arg(error));
        m_empty->show();
        setSubtitle(QString());
        m_current = 0;
        showAccount();
        return;
    }

    m_accounts = db.users();
    setSubtitle(QStringLiteral("%1 %2 на сервере.")
                    .arg(Look::number(m_accounts.size()))
                    .arg(Look::plural(m_accounts.size(), QStringLiteral("аккаунт"),
                                      QStringLiteral("аккаунта"), QStringLiteral("аккаунтов"))));

    const QString filter = m_search->text().trimmed();
    const qint64 keep = m_current;
    m_users->setSortingEnabled(false);
    m_users->clear();
    QTreeWidgetItem *selected = nullptr;
    for (const UserSummary &user : m_accounts) {
        if (!filter.isEmpty() && !user.login.contains(filter, Qt::CaseInsensitive)
            && !user.displayName.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        auto *item = new SortItem(m_users);
        item->setData(0, kIdRole, user.id);
        item->setText(0, user.login);
        item->setText(1, user.displayName);
        const QDateTime last = user.lastSync.isValid() ? user.lastSync : user.lastSeen;
        item->setText(2, Look::ago(last));
        setSort(item, 2, last.isValid() ? double(last.toMSecsSinceEpoch()) : 0.0);
        if (user.id == keep)
            selected = item;
    }
    m_users->setSortingEnabled(true);

    m_empty->setText(m_accounts.isEmpty()
                         ? QStringLiteral("Аккаунтов пока нет: их создают планеры при регистрации.")
                         : QStringLiteral("Никто не подходит под «%1».").arg(filter));
    m_empty->setVisible(m_users->topLevelItemCount() == 0);

    if (!selected && m_users->topLevelItemCount() > 0)
        selected = m_users->topLevelItem(0);
    if (selected) {
        // The account is shown once, below, whether the row changed or not.
        const QSignalBlocker quiet(m_users);
        m_users->setCurrentItem(selected);
        m_current = selected->data(0, kIdRole).toLongLong();
    } else {
        m_current = 0;
    }
    showAccount();
}

void UsersPage::showAccount()
{
    const UserSummary *user = nullptr;
    for (const UserSummary &candidate : m_accounts) {
        if (candidate.id == m_current)
            user = &candidate;
    }
    m_details->setEnabled(user != nullptr);
    m_records.clear();
    m_calendarNames.clear();
    for (QTreeWidget *tree : { m_devices, m_calendars, m_events, m_tasks, m_other })
        tree->clear();
    if (!user) {
        m_login->setText(QStringLiteral("Аккаунт не выбран"));
        m_name->clear();
        for (QLabel *label : { m_created, m_lastSync, m_syncs, m_volume })
            label->setText(QStringLiteral("—"));
        fillRecords();
        return;
    }

    AdminDb &db = session()->db();
    m_login->setText(user->login);
    m_name->setText(user->displayName.isEmpty() ? QStringLiteral("без имени") : user->displayName);
    m_created->setText(Look::moment(user->createdAt));
    if (user->lastSync.isValid()) {
        m_lastSync->setText(QStringLiteral("%1 (%2)").arg(Look::moment(user->lastSync),
                                                          Look::ago(user->lastSync)));
    } else if (user->lastSeen.isValid()) {
        m_lastSync->setText(QStringLiteral("не учтён; последний раз устройство заходило %1")
                                .arg(Look::ago(user->lastSeen)));
    } else {
        m_lastSync->setText(QStringLiteral("ни одного"));
    }
    m_syncs->setText(db.tracksActivity() ? Look::number(user->syncsMonth)
                                         : QStringLiteral("база не ведёт счёт обменов"));
    m_volume->setText(QStringLiteral("%1 %2, %3 · изменений на сервере: %4")
                          .arg(Look::number(user->records))
                          .arg(Look::plural(user->records, QStringLiteral("запись"),
                                            QStringLiteral("записи"), QStringLiteral("записей")))
                          .arg(Look::bytes(user->bytes))
                          .arg(Look::number(user->revision)));

    // Devices.
    const QList<DeviceSession> devices = db.devices(user->id);
    for (const DeviceSession &device : devices) {
        auto *item = new SortItem(m_devices);
        item->setText(0, device.name.isEmpty() ? QStringLiteral("без имени") : device.name);
        setMoment(item, 1, device.createdAt);
        item->setText(2, Look::ago(device.lastUsedAt));
        setSort(item, 2, device.lastUsedAt.isValid() ? double(device.lastUsedAt.toMSecsSinceEpoch()) : 0.0);
    }
    m_tabs->setTabText(4, QStringLiteral("Устройства (%1)").arg(devices.size()));

    m_records = db.records(user->id, true);

    // Calendars, with the members of the opened ones beneath them.
    const QList<CalendarSummary> calendars = db.calendars(user->id, m_records);
    m_calendars->setSortingEnabled(false);
    for (const CalendarSummary &calendar : calendars) {
        m_calendarNames.insert(calendar.id, calendar.name);
        auto *item = new SortItem(m_calendars);
        item->setIcon(0, colorDot(calendar.color));
        item->setText(0, calendar.missing ? QStringLiteral("(запись календаря ещё не пришла)")
                                          : calendar.name);
        QString role;
        switch (calendar.role) {
        case CalendarSummary::Role::Own:
            role = QStringLiteral("свой");
            break;
        case CalendarSummary::Role::Open:
            role = calendar.enabled
                       ? QStringLiteral("свой, открыт: %1 %2, %3")
                             .arg(calendar.members.size())
                             .arg(Look::plural(calendar.members.size(), QStringLiteral("участник"),
                                               QStringLiteral("участника"), QStringLiteral("участников")))
                             .arg(modeText(calendar.mode))
                       : QStringLiteral("свой, доступ закрыт");
            break;
        case CalendarSummary::Role::Twin:
            role = QStringLiteral("близнец календаря «%1» у %2, доступ: %3")
                       .arg(calendar.originName.isEmpty() ? QStringLiteral("…") : calendar.originName,
                            calendar.ownerLogin, accessText(calendar.access));
            break;
        }
        item->setText(1, role);
        item->setToolTip(1, role);
        item->setText(2, Look::number(calendar.events));
        setSort(item, 2, calendar.events);
        item->setText(3, Look::number(calendar.tasks));
        setSort(item, 3, calendar.tasks);
        item->setText(4, Look::number(calendar.other));
        setSort(item, 4, calendar.other);
        for (int column : { 2, 3, 4 })
            item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
        for (const ShareMemberInfo &member : calendar.members) {
            auto *row = new SortItem(item);
            row->setText(0, member.displayName.isEmpty()
                                ? member.login
                                : QStringLiteral("%1 (%2)").arg(member.login, member.displayName));
            row->setText(1, QStringLiteral("участник с %1, доступ: %2")
                                .arg(member.joinedAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy")),
                                     accessText(member.access)));
            row->setForeground(0, Look::secondary());
            row->setForeground(1, Look::secondary());
        }
        item->setExpanded(true);
    }
    m_calendars->setSortingEnabled(true);
    m_calendars->sortByColumn(0, Qt::AscendingOrder);
    m_tabs->setTabText(0, QStringLiteral("Календари (%1)").arg(calendars.size()));

    fillRecords();
}

void UsersPage::fillRecords()
{
    const QString filter = m_recordSearch->text().trimmed();
    const bool withDeleted = m_deleted->isChecked();
    int events = 0;
    int tasks = 0;
    int other = 0;

    for (QTreeWidget *tree : { m_events, m_tasks, m_other }) {
        tree->setSortingEnabled(false);
        tree->clear();
    }
    for (int index = 0; index < m_records.size(); ++index) {
        const StoredRecord &record = m_records.at(index);
        if (record.kind == QLatin1String("calendar"))
            continue;
        if (record.deleted && !withDeleted)
            continue;
        const QString title = PlannerRecord::title(record.kind, record.data);
        if (!filter.isEmpty() && !title.contains(filter, Qt::CaseInsensitive))
            continue;

        QTreeWidget *tree = record.kind == QLatin1String("event") ? m_events
                            : record.kind == QLatin1String("task") ? m_tasks
                                                                   : m_other;
        auto *item = new SortItem(tree);
        item->setData(0, kIdRole, index);
        item->setText(0, record.deleted ? QStringLiteral("(удалена)")
                                        : title.isEmpty() ? QStringLiteral("—") : title);
        const QDateTime modified = QDateTime::fromMSecsSinceEpoch(record.modified);
        bool allDay = false;
        const QDateTime moment = PlannerRecord::moment(record.kind, record.data, &allDay);
        const QString when = PlannerRecord::momentText(record.kind, record.data);
        const QString calendar = m_calendarNames.value(PlannerRecord::calendarId(record.data),
                                                       PlannerRecord::calendarId(record.data));
        const QString state = record.deleted ? QStringLiteral("удалена")
                                             : PlannerRecord::state(record.kind, record.data);
        if (tree == m_other) {
            item->setText(1, PlannerRecord::kindName(record.kind));
            item->setText(2, when);
            setSort(item, 2, moment.isValid() ? double(moment.toMSecsSinceEpoch()) : 0.0);
            setMoment(item, 3, modified);
            item->setText(4, record.device);
            ++other;
        } else {
            item->setText(1, when);
            setSort(item, 1, moment.isValid() ? double(moment.toMSecsSinceEpoch()) : 0.0);
            item->setText(2, calendar);
            item->setText(3, state);
            setMoment(item, 4, modified);
            item->setText(5, record.device);
            ++(tree == m_events ? events : tasks);
        }
        if (record.deleted) {
            QFont font = item->font(0);
            font.setItalic(true);
            for (int column = 0; column < tree->columnCount(); ++column) {
                item->setFont(column, font);
                item->setForeground(column, Look::muted());
            }
        }
    }
    for (QTreeWidget *tree : { m_events, m_tasks, m_other }) {
        tree->setSortingEnabled(true);
        tree->sortByColumn(tree == m_other ? 3 : 1, Qt::DescendingOrder);
    }
    m_tabs->setTabText(1, QStringLiteral("События (%1)").arg(events));
    m_tabs->setTabText(2, QStringLiteral("Задачи (%1)").arg(tasks));
    m_tabs->setTabText(3, QStringLiteral("Прочее (%1)").arg(other));
}

void UsersPage::openRecord(QTreeWidgetItem *item)
{
    const int index = item ? item->data(0, kIdRole).toInt() : -1;
    if (index < 0 || index >= m_records.size())
        return;
    RecordDialog dialog(m_records.at(index), this);
    dialog.exec();
}

void UsersPage::changePassword()
{
    const UserSummary *user = nullptr;
    for (const UserSummary &candidate : m_accounts) {
        if (candidate.id == m_current)
            user = &candidate;
    }
    if (!user)
        return;
    PasswordDialog dialog(*user, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    QString error;
    if (!session()->db().setPassword(user->id, dialog.password(), &error)) {
        QMessageBox::warning(this, QStringLiteral("Новый пароль"),
                             QStringLiteral("Пароль не изменён: %1").arg(error));
        return;
    }
    QMessageBox::information(this, QStringLiteral("Новый пароль"),
                             QStringLiteral("Пароль аккаунта «%1» изменён. Его устройства выйдут "
                                            "из аккаунта при следующем обмене.")
                                 .arg(user->login));
    emit session()->accountsChanged();
}
