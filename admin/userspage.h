#pragma once

#include "admindb.h"
#include "page.h"

#include <QHash>
#include <QList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

/*!
 * "Пользователи": the accounts, and for the chosen one its devices, its
 * calendars (its own, the opened ones with their members, and the twins of
 * calendars of others), its events, tasks and the rest of its records, each
 * of them openable as the JSON it is stored as. A new password is set here.
 */
class UsersPage : public Page
{
    Q_OBJECT

public:
    explicit UsersPage(AdminSession *session, QWidget *parent = nullptr);

    void activate() override;

    /*! For --grab: the number of tabs of an account, and to show one of them. */
    int tabCount() const;
    QString tabName(int index) const;
    void showFirstAccount(int tab);

    /*! For --grab: the record and password dialogs of the first account, into @p folder. */
    void grabDialogs(const QString &folder);

private:
    void reload();
    void showAccount();
    void fillRecords();
    void openRecord(QTreeWidgetItem *item);
    void changePassword();

    QLineEdit *m_search = nullptr;
    QTreeWidget *m_users = nullptr;
    QLabel *m_empty = nullptr;

    QWidget *m_details = nullptr;
    QLabel *m_login = nullptr;
    QLabel *m_name = nullptr;
    QLabel *m_created = nullptr;
    QLabel *m_lastSync = nullptr;
    QLabel *m_syncs = nullptr;
    QLabel *m_volume = nullptr;
    QPushButton *m_password = nullptr;
    QLineEdit *m_recordSearch = nullptr;
    QCheckBox *m_deleted = nullptr;
    QTabWidget *m_tabs = nullptr;
    QTreeWidget *m_devices = nullptr;
    QTreeWidget *m_calendars = nullptr;
    QTreeWidget *m_events = nullptr;
    QTreeWidget *m_tasks = nullptr;
    QTreeWidget *m_other = nullptr;

    QList<UserSummary> m_accounts;
    qint64 m_current = 0;
    QList<StoredRecord> m_records;
    QHash<QString, QString> m_calendarNames;
};
