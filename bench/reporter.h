#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

/*!
 * The progress of the benchmark, one event a line on stdout: JSON for the
 * admin panel (--json), plain lines for a console.
 *
 * Events (field "event"):
 *   plan        {stages: [{id, title}], runs, estimatedSeconds}
 *   stage       {index, id, title}
 *   progress    {percent, message, elapsed}
 *   heartbeat   {percent, message, elapsed, idle} - every second, whatever
 *               happens; idle is how long ago the last progress was
 *   system      {cores, memoryTotal, ...}  (SystemInfo)
 *   run         {stage, label, scenario, settings, result}
 *   note        {text}
 *   recommendation {settings, current, gainPercent, notes}
 *   done        {result}   - the path of result.json
 *   error       {message}
 *   cancelled   {}
 *
 * The heartbeat comes from a thread of its own: it says the process is
 * there, "idle" says whether it moves.
 */
class Reporter
{
public:
    /*! Lines go to stdout, or to @p sink when there is one (the tests). */
    explicit Reporter(bool json, std::function<void(const QByteArray &line)> sink = {});
    ~Reporter();

    Reporter(const Reporter &) = delete;
    Reporter &operator=(const Reporter &) = delete;

    void plan(const QList<QPair<QString, QString>> &stages, int runs, int estimatedSeconds);
    void stage(int index, const QString &id, const QString &title);
    void progress(double percent, const QString &message);
    void event(const QString &name, const QJsonObject &fields, const QString &text = QString());

    double elapsedSeconds() const;

private:
    void write(const QJsonObject &object, const QString &text);
    void beat();

    const bool m_json;
    std::function<void(const QByteArray &)> m_sink;
    mutable std::mutex m_lock;
    qint64 m_startedMs = 0;
    qint64 m_lastProgressMs = 0;
    qint64 m_lastTextMs = 0;
    double m_percent = 0;
    QString m_message;
    std::atomic<bool> m_stop { false };
    std::thread m_heartbeat;
};
