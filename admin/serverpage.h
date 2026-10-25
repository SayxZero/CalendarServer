#pragma once

#include "page.h"

class QLabel;
class QPushButton;

/*!
 * "Сервер": whether the server runs, starting and stopping its service,
 * whether it answers, the addresses for the planners and where its files lie.
 */
class ServerPage : public Page
{
    Q_OBJECT

public:
    explicit ServerPage(AdminSession *session, QWidget *parent = nullptr);

    void activate() override;

private:
    void showStatus();
    void showPing();
    void showSettings();

    /*! Starts or stops the service and waits for it; @p restart does both. */
    void control(bool start, bool restart = false);

    /*! The newest error lines of the log, for the message after a failed start. */
    QString recentProblems() const;

    QLabel *m_state = nullptr;
    QLabel *m_details = nullptr;
    QLabel *m_ping = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_stop = nullptr;
    QPushButton *m_restart = nullptr;

    QLabel *m_port = nullptr;
    QLabel *m_registration = nullptr;
    QLabel *m_addresses = nullptr;

    QLabel *m_database = nullptr;
    QLabel *m_dataDir = nullptr;
    QLabel *m_configFile = nullptr;
    QLabel *m_logFile = nullptr;
    QPushButton *m_editConfig = nullptr;
};
