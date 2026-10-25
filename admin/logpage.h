#pragma once

#include "page.h"

#include <QDateTime>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTimer;

/*!
 * "Журнал": the end of server.log, followed as it grows. The service has no
 * console, and this is where it says what it does and why it stopped.
 */
class LogPage : public Page
{
    Q_OBJECT

public:
    explicit LogPage(AdminSession *session, QWidget *parent = nullptr);

    void activate() override;

protected:
    void hideEvent(QHideEvent *event) override;

private:
    void reload(bool force = false);

    QLabel *m_path = nullptr;
    QCheckBox *m_follow = nullptr;
    QCheckBox *m_problems = nullptr;
    QPushButton *m_open = nullptr;
    QPlainTextEdit *m_text = nullptr;
    QTimer *m_timer = nullptr;
    qint64 m_size = -1;
    QDateTime m_modified;
};
