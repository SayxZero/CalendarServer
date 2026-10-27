#pragma once

#include "loadhistory.h"
#include "page.h"

class Chart;
class QFrame;
class QButtonGroup;
class QGridLayout;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTimer;
class QTreeWidget;

/*!
 * "Нагрузка": how busy the server is right now - requests, the line in front
 * of its threads and the wait in it, the processor and the memory - and the
 * same over an hour, a day, a week, a month or a year, from metrics.db.
 * While the page is shown it reads them again every five seconds.
 */
class LoadPage : public Page
{
    Q_OBJECT

public:
    explicit LoadPage(AdminSession *session, QWidget *parent = nullptr);

    void activate() override;

    /*! For --grab: shows the table instead of the charts. */
    void showTable(bool table);

protected:
    void hideEvent(QHideEvent *event) override;

private:
    struct Tile
    {
        QLabel *value = nullptr;
        QLabel *note = nullptr;
    };

    Tile addTile(int row, int column, const QString &label);
    QFrame *chartCard(const QString &title, const QString &caption, Chart *chart);
    void reload();
    void showNow(const LoadNow &now, const QList<LoadPoint> &lastHour);
    void showHistory(const QList<LoadPoint> &points);
    void showThreads(const LoadNow &now);
    LoadPeriod period() const;

    LoadHistory m_history;
    QTimer *m_refresh = nullptr;
    QButtonGroup *m_period = nullptr;
    QPushButton *m_tableToggle = nullptr;
    QLabel *m_updated = nullptr;
    QLabel *m_state = nullptr;

    QGridLayout *m_tiles = nullptr;
    Tile m_requests;
    Tile m_queue;
    Tile m_wait;
    Tile m_work;
    Tile m_cpu;
    Tile m_memory;
    Tile m_connections;
    Tile m_refused;

    QStackedWidget *m_views = nullptr;
    Chart *m_requestChart = nullptr;
    Chart *m_queueChart = nullptr;
    Chart *m_timeChart = nullptr;
    Chart *m_cpuChart = nullptr;
    Chart *m_memoryChart = nullptr;
    Chart *m_connectionChart = nullptr;
    QTreeWidget *m_rows = nullptr;

    Chart *m_threadChart = nullptr;
    QLabel *m_threadNote = nullptr;
    QTreeWidget *m_threadRows = nullptr;
};
