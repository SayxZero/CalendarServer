#pragma once

#include "backups.h"
#include "page.h"

class QLabel;
class QPushButton;
class QTreeWidget;

/*!
 * "Резервные копии": copies of the base taken on the fly, the folder they go
 * to, and the way back from one of them - which stops the server for the
 * moment of the swap and starts it again.
 */
class BackupPage : public Page
{
    Q_OBJECT

public:
    explicit BackupPage(AdminSession *session, QWidget *parent = nullptr);

    void activate() override;

private:
    void reload();
    void create();
    void restoreSelected();
    void restoreFromFile();
    void restore(const QString &path);
    void exportSelected();
    void chooseFolder();
    QString selectedPath() const;

    QLabel *m_folder = nullptr;
    QLabel *m_summary = nullptr;
    QTreeWidget *m_list = nullptr;
    QPushButton *m_restore = nullptr;
    QPushButton *m_export = nullptr;
    QList<BackupFile> m_backups;
};
