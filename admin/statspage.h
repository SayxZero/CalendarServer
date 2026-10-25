#pragma once

#include "page.h"

class Chart;
class QButtonGroup;
class QGridLayout;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTreeWidget;

/*!
 * "Аналитика": accounts, how many of them sync a day, a week and a month,
 * with the history of it on a chart; exchanges, devices, records, shared
 * calendars and where the space of the base goes.
 */
class StatsPage : public Page
{
    Q_OBJECT

public:
    explicit StatsPage(AdminSession *session, QWidget *parent = nullptr);

    void activate() override;

    /*! For --grab: shows the tables instead of the charts. */
    void showTables(bool tables);

private:
    struct Tile
    {
        QLabel *value = nullptr;
        QLabel *note = nullptr;
    };

    Tile addTile(int row, int column, const QString &label);
    void reload();
    int days() const;

    QButtonGroup *m_period = nullptr;
    QLabel *m_updated = nullptr;
    QLabel *m_tracking = nullptr;

    Tile m_accounts;
    Tile m_day;
    Tile m_week;
    Tile m_month;
    Tile m_syncs;
    Tile m_devices;
    Tile m_records;
    Tile m_storage;
    QGridLayout *m_tiles = nullptr;

    QPushButton *m_activeTable = nullptr;
    QStackedWidget *m_activeStack = nullptr;
    Chart *m_active = nullptr;
    QTreeWidget *m_activeRows = nullptr;

    QLabel *m_syncsTitle = nullptr;
    QPushButton *m_syncsTable = nullptr;
    QStackedWidget *m_syncsStack = nullptr;
    Chart *m_exchanges = nullptr;
    QTreeWidget *m_exchangeRows = nullptr;

    QLabel *m_file = nullptr;
    QLabel *m_payload = nullptr;
    QLabel *m_free = nullptr;
    QLabel *m_traffic = nullptr;
    QLabel *m_shares = nullptr;
    QTreeWidget *m_kinds = nullptr;
    QTreeWidget *m_top = nullptr;
};
