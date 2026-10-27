#pragma once

#include <QList>
#include <QMainWindow>

class AdminSession;
class Page;
class QLabel;
class QListWidget;
class QStackedWidget;
class LoadPage;
class ThreadsPage;
class StatsPage;
class UsersPage;

/*!
 * The window of the admin panel: the pages in a list on the left, as in the
 * planner, the chosen one on the right and the state of the server under the
 * list, whichever page is open.
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(AdminSession *session, QWidget *parent = nullptr);

    /*!
     * --grab: every page (and every tab of an account) into a PNG file of
     * @p folder - how the panel is checked without anybody clicking it.
     */
    void grabPages(const QString &folder);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void showPage(int index);
    void showStatus();

    AdminSession *m_session;
    QListWidget *m_nav = nullptr;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_answer = nullptr;
    QList<Page *> m_pages;
    UsersPage *m_users = nullptr;
    StatsPage *m_stats = nullptr;
    LoadPage *m_load = nullptr;
    ThreadsPage *m_threads = nullptr;
};
