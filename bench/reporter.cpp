#include "reporter.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>

#include <chrono>
#include <cstdio>

namespace {

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

Reporter::Reporter(bool json, std::function<void(const QByteArray &)> sink)
    : m_json(json)
    , m_sink(std::move(sink))
    , m_startedMs(nowMs())
    , m_lastProgressMs(m_startedMs)
{
    m_heartbeat = std::thread([this] { beat(); });
}

Reporter::~Reporter()
{
    m_stop = true;
    if (m_heartbeat.joinable())
        m_heartbeat.join();
}

double Reporter::elapsedSeconds() const
{
    return double(nowMs() - m_startedMs) / 1000;
}

void Reporter::write(const QJsonObject &object, const QString &text)
{
    // Under the lock already: one line at a time, whole.
    QByteArray line;
    if (m_json) {
        line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    } else {
        if (text.isEmpty())
            return;
        line = QStringLiteral("%1 %2\n")
                   .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), text)
                   .toUtf8();
    }
    if (m_sink) {
        m_sink(line);
        return;
    }
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

void Reporter::plan(const QList<QPair<QString, QString>> &stages, int runs, int estimatedSeconds)
{
    QJsonArray list;
    QStringList titles;
    for (const auto &stage : stages) {
        QJsonObject one;
        one.insert(QStringLiteral("id"), stage.first);
        one.insert(QStringLiteral("title"), stage.second);
        list.append(one);
        titles << stage.second;
    }
    QJsonObject fields;
    fields.insert(QStringLiteral("stages"), list);
    fields.insert(QStringLiteral("runs"), runs);
    fields.insert(QStringLiteral("estimatedSeconds"), estimatedSeconds);
    event(QStringLiteral("plan"), fields,
          QStringLiteral("План: %1; прогонов %2, около %3 мин")
              .arg(titles.join(QStringLiteral(", ")))
              .arg(runs)
              .arg(qMax(1, (estimatedSeconds + 30) / 60)));
}

void Reporter::stage(int index, const QString &id, const QString &title)
{
    QJsonObject fields;
    fields.insert(QStringLiteral("index"), index);
    fields.insert(QStringLiteral("id"), id);
    fields.insert(QStringLiteral("title"), title);
    event(QStringLiteral("stage"), fields, QStringLiteral("=== %1. %2").arg(index + 1).arg(title));
}

void Reporter::progress(double percent, const QString &message)
{
    std::lock_guard<std::mutex> guard(m_lock);
    m_percent = qBound(0.0, percent, 100.0);
    m_message = message;
    m_lastProgressMs = nowMs();
    QJsonObject object;
    object.insert(QStringLiteral("event"), QStringLiteral("progress"));
    object.insert(QStringLiteral("percent"), m_percent);
    object.insert(QStringLiteral("message"), message);
    object.insert(QStringLiteral("elapsed"), double(m_lastProgressMs - m_startedMs) / 1000);
    // A console gets a line every few seconds, not every tick.
    QString text;
    if (m_lastProgressMs - m_lastTextMs >= 5000) {
        m_lastTextMs = m_lastProgressMs;
        text = QStringLiteral("    %1 % · %2").arg(qRound(m_percent)).arg(message);
    }
    write(object, text);
}

void Reporter::event(const QString &name, const QJsonObject &fields, const QString &text)
{
    std::lock_guard<std::mutex> guard(m_lock);
    QJsonObject object = fields;
    object.insert(QStringLiteral("event"), name);
    object.insert(QStringLiteral("elapsed"), double(nowMs() - m_startedMs) / 1000);
    write(object, text);
}

void Reporter::beat()
{
    while (!m_stop.load()) {
        for (int i = 0; i < 10 && !m_stop.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (m_stop.load() || !m_json)
            continue;
        std::lock_guard<std::mutex> guard(m_lock);
        const qint64 now = nowMs();
        QJsonObject object;
        object.insert(QStringLiteral("event"), QStringLiteral("heartbeat"));
        object.insert(QStringLiteral("percent"), m_percent);
        object.insert(QStringLiteral("message"), m_message);
        object.insert(QStringLiteral("elapsed"), double(now - m_startedMs) / 1000);
        object.insert(QStringLiteral("idle"), double(now - m_lastProgressMs) / 1000);
        write(object, QString());
    }
}
