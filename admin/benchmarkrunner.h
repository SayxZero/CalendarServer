#pragma once

#include "loadrun.h"
#include "probe.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

/*!
 * CalendarServerBench as the admin panel runs it: a process of its own,
 * whose JSON lines (bench/reporter.h) turn into the state below. A process
 * of its own is what makes it stoppable at any moment: stop() kills it, and
 * the servers it started die with the job object they run in.
 */
class BenchmarkRunner : public QObject
{
    Q_OBJECT

public:
    enum class Outcome { Idle, Running, Done, Failed, Cancelled };

    struct Run
    {
        QString stage;
        QString label;
        QString scenario;
        ServerSettings settings;
        LoadResult result;
    };

    explicit BenchmarkRunner(QObject *parent = nullptr);
    ~BenchmarkRunner() override;

    /*! CalendarServerBench next to the panel. */
    static QString programPath();

    /*! Starts the benchmark with @p arguments ("tune --json ..."); everything known is reset. */
    bool start(const QStringList &arguments, QString *error = nullptr);
    void stop();
    bool isRunning() const { return m_outcome == Outcome::Running; }
    Outcome outcome() const { return m_outcome; }

    /*! One line of its output; false for a line that is no event. Public for the tests. */
    bool handleLine(const QByteArray &line);

    // ---- what is known -------------------------------------------------------
    QList<QPair<QString, QString>> stages() const { return m_stages; }
    int stageIndex() const { return m_stage; }
    int runsPlanned() const { return m_runsPlanned; }
    int estimatedSeconds() const { return m_estimatedSeconds; }
    double percent() const { return m_percent; }
    QString message() const { return m_message; }
    double elapsed() const { return m_elapsed; }

    /*! Seconds since the benchmark last moved (its own heartbeat says). */
    double idle() const { return m_idle; }

    /*! Seconds since its last line of any kind: the heartbeat is every second. */
    double silence() const;

    const QList<Run> &runs() const { return m_runs; }
    bool haveSystem() const { return m_haveSystem; }
    const SystemInfo &system() const { return m_system; }
    QJsonObject recommendation() const { return m_recommendation; }
    QString resultPath() const { return m_resultPath; }
    QString error() const { return m_error; }

    /*! What it said, for people: stages, runs, notes. */
    QStringList log() const { return m_log; }

signals:
    void changed();
    void finished();

private:
    void readOutput();
    void processEnded(int exitCode);

    QProcess *m_process = nullptr;
    QByteArray m_buffer;
    QByteArray m_errors;
    bool m_stopping = false;
    Outcome m_outcome = Outcome::Idle;
    QList<QPair<QString, QString>> m_stages;
    int m_stage = -1;
    int m_runsPlanned = 0;
    int m_estimatedSeconds = 0;
    double m_percent = 0;
    QString m_message;
    double m_elapsed = 0;
    double m_idle = 0;
    QDateTime m_lastLine;
    QList<Run> m_runs;
    bool m_haveSystem = false;
    SystemInfo m_system;
    QJsonObject m_recommendation;
    QString m_resultPath;
    QString m_error;
    QStringList m_log;
};
