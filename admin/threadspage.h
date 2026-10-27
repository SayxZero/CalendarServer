#pragma once

#include "page.h"

#include <QJsonObject>

class BenchmarkRunner;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;
class QTreeWidget;
struct ServerSettings;

/*!
 * "Многопоточность": the load settings of the server - threads, the queue
 * to them, group commit, fsync, connections, memory, the wait in line - and
 * the benchmark that picks them for this machine (CalendarServerBench tune).
 *
 * Both only while the server is stopped: settings change at its start, and a
 * benchmark next to a working server would measure the two of them together.
 * The benchmark shows its stage, what it does and how long ago it last
 * moved, and stops at any moment.
 */
class ThreadsPage : public Page
{
    Q_OBJECT

public:
    explicit ThreadsPage(AdminSession *session, QWidget *parent = nullptr);
    ~ThreadsPage() override;

    void activate() override;

private:
    QString settingsPath() const;
    bool serverStopped() const;
    void loadSettings();
    void showSettings(const ServerSettings &settings);
    ServerSettings formSettings() const;
    void saveSettings();
    void applyRecommendation();
    void startBenchmark();
    void showState();
    void showProgress();
    void showResult(const QJsonObject &document);
    void loadLastResult();
    QString workFolder() const;

    BenchmarkRunner *m_runner = nullptr;
    QTimer *m_tick = nullptr;

    QLabel *m_state = nullptr;
    QPushButton *m_stopServer = nullptr;

    QSpinBox *m_threads = nullptr;
    QComboBox *m_dispatch = nullptr;
    QSpinBox *m_groupCommit = nullptr;
    QComboBox *m_syncMode = nullptr;
    QSpinBox *m_maxConnections = nullptr;
    QSpinBox *m_bufferMb = nullptr;
    QSpinBox *m_queueWait = nullptr;
    QLabel *m_file = nullptr;
    QLabel *m_saved = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_revert = nullptr;
    QPushButton *m_defaults = nullptr;
    QPushButton *m_recommended = nullptr;
    QList<QWidget *> m_fields;

    QComboBox *m_mode = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_stop = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_stage = nullptr;
    QLabel *m_step = nullptr;
    QLabel *m_alive = nullptr;
    QPlainTextEdit *m_log = nullptr;

    QLabel *m_resultTitle = nullptr;
    QLabel *m_system = nullptr;
    QLabel *m_advice = nullptr;
    QTreeWidget *m_runs = nullptr;
    QJsonObject m_recommendation;
};
