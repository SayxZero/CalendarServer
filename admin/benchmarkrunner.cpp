#include "benchmarkrunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QProcess>

namespace {

QString summary(const LoadResult &result)
{
    const QLocale russian(QLocale::Russian);
    if (!result.started)
        return QStringLiteral("не прошёл: %1").arg(result.error);
    QString text = QStringLiteral("%1 запросов/с, p50 %2 мс, p95 %3 мс")
                       .arg(qRound(result.throughput))
                       .arg(russian.toString(result.p50, 'f', 1), russian.toString(result.p95, 'f', 1));
    if (result.refused > 0)
        text += QStringLiteral(", отказов %1").arg(result.refused);
    if (result.errors > 0)
        text += QStringLiteral(", ошибок %1").arg(result.errors);
    if (result.died)
        text += QStringLiteral(", сервер упал");
    return text;
}

} // namespace

BenchmarkRunner::BenchmarkRunner(QObject *parent)
    : QObject(parent)
{
}

BenchmarkRunner::~BenchmarkRunner()
{
    // A benchmark the panel no longer shows has nobody to report to.
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(5000);
    }
}

QString BenchmarkRunner::programPath()
{
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("CalendarServerBench.exe");
#else
    const QString name = QStringLiteral("CalendarServerBench");
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

bool BenchmarkRunner::start(const QStringList &arguments, QString *error)
{
    if (isRunning())
        return false;
    const QString program = programPath();
    if (!QFile::exists(program)) {
        if (error)
            *error = QStringLiteral("Нет программы бенчмарка: %1").arg(QDir::toNativeSeparators(program));
        return false;
    }
    m_stages.clear();
    m_stage = -1;
    m_runsPlanned = 0;
    m_estimatedSeconds = 0;
    m_percent = 0;
    m_message.clear();
    m_elapsed = 0;
    m_idle = 0;
    m_runs.clear();
    m_haveSystem = false;
    m_system = SystemInfo();
    m_recommendation = QJsonObject();
    m_resultPath.clear();
    m_error.clear();
    m_log.clear();
    m_buffer.clear();
    m_errors.clear();
    m_stopping = false;

    delete m_process;
    m_process = new QProcess(this);
    m_process->setProgram(program);
    m_process->setArguments(arguments);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &BenchmarkRunner::readOutput);
    connect(m_process, &QProcess::readyReadStandardError, this,
            [this] { m_errors += m_process->readAllStandardError(); });
    connect(m_process, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus) { processEnded(exitCode); });
    m_process->start();
    if (!m_process->waitForStarted(10000)) {
        if (error)
            *error = m_process->errorString();
        return false;
    }
    m_outcome = Outcome::Running;
    m_lastLine = QDateTime::currentDateTime();
    m_log << QStringLiteral("Запущен бенчмарк: %1").arg(arguments.join(QLatin1Char(' ')));
    emit changed();
    return true;
}

void BenchmarkRunner::stop()
{
    if (!m_process || !isRunning())
        return;
    m_stopping = true;
    m_log << QStringLiteral("Остановлено по просьбе администратора.");
    m_process->kill();
}

double BenchmarkRunner::silence() const
{
    return m_lastLine.isValid() ? double(m_lastLine.msecsTo(QDateTime::currentDateTime())) / 1000 : 0;
}

void BenchmarkRunner::readOutput()
{
    m_buffer += m_process->readAllStandardOutput();
    for (;;) {
        const int end = m_buffer.indexOf('\n');
        if (end < 0)
            break;
        const QByteArray line = m_buffer.left(end).trimmed();
        m_buffer.remove(0, end + 1);
        if (!line.isEmpty())
            handleLine(line);
    }
}

bool BenchmarkRunner::handleLine(const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonObject event = QJsonDocument::fromJson(line, &parseError).object();
    if (parseError.error != QJsonParseError::NoError || !event.contains(QStringLiteral("event")))
        return false;
    m_lastLine = QDateTime::currentDateTime();
    if (event.contains(QStringLiteral("elapsed")))
        m_elapsed = event.value(QStringLiteral("elapsed")).toDouble();
    const QString name = event.value(QStringLiteral("event")).toString();

    if (name == QLatin1String("plan")) {
        m_stages.clear();
        for (const QJsonValue &value : event.value(QStringLiteral("stages")).toArray()) {
            const QJsonObject stage = value.toObject();
            m_stages << qMakePair(stage.value(QStringLiteral("id")).toString(),
                                  stage.value(QStringLiteral("title")).toString());
        }
        m_runsPlanned = event.value(QStringLiteral("runs")).toInt();
        m_estimatedSeconds = event.value(QStringLiteral("estimatedSeconds")).toInt();
        m_log << QStringLiteral("Этапов %1, прогонов %2, около %3 мин.")
                     .arg(m_stages.size())
                     .arg(m_runsPlanned)
                     .arg(qMax(1, (m_estimatedSeconds + 30) / 60));
    } else if (name == QLatin1String("stage")) {
        m_stage = event.value(QStringLiteral("index")).toInt();
        m_log << QStringLiteral("Этап %1 из %2: %3")
                     .arg(m_stage + 1)
                     .arg(m_stages.size())
                     .arg(event.value(QStringLiteral("title")).toString());
    } else if (name == QLatin1String("progress") || name == QLatin1String("heartbeat")) {
        m_percent = event.value(QStringLiteral("percent")).toDouble();
        m_message = event.value(QStringLiteral("message")).toString();
        m_idle = name == QLatin1String("heartbeat") ? event.value(QStringLiteral("idle")).toDouble() : 0;
    } else if (name == QLatin1String("system")) {
        m_system = SystemInfo::fromJson(event);
        m_haveSystem = true;
        for (const QString &text : m_system.describe())
            m_log << QStringLiteral("    ") + text;
    } else if (name == QLatin1String("run")) {
        Run run;
        run.stage = event.value(QStringLiteral("stage")).toString();
        run.label = event.value(QStringLiteral("label")).toString();
        run.scenario = event.value(QStringLiteral("scenario")).toString();
        run.settings = ServerSettings::fromJson(event.value(QStringLiteral("settings")).toObject());
        run.result = LoadResult::fromJson(event.value(QStringLiteral("result")).toObject());
        m_runs << run;
        m_log << QStringLiteral("    %1: %2").arg(run.label, summary(run.result));
    } else if (name == QLatin1String("note")) {
        m_log << QStringLiteral("    ") + event.value(QStringLiteral("text")).toString();
    } else if (name == QLatin1String("recommendation")) {
        m_recommendation = event;
        m_recommendation.remove(QStringLiteral("event"));
        m_recommendation.remove(QStringLiteral("elapsed"));
        m_log << QStringLiteral("Рекомендация: %1")
                     .arg(ServerSettings::fromJson(event.value(QStringLiteral("settings")).toObject())
                              .describe());
        for (const QJsonValue &note : event.value(QStringLiteral("notes")).toArray())
            m_log << QStringLiteral("    ") + note.toString();
    } else if (name == QLatin1String("done")) {
        m_resultPath = event.value(QStringLiteral("result")).toString();
        m_percent = 100;
        m_log << QStringLiteral("Готово.");
    } else if (name == QLatin1String("error")) {
        m_error = event.value(QStringLiteral("message")).toString();
        m_log << QStringLiteral("Ошибка: %1").arg(m_error);
    } else if (name == QLatin1String("cancelled")) {
        m_stopping = true;
    }
    emit changed();
    return true;
}

void BenchmarkRunner::processEnded(int exitCode)
{
    readOutput();
    if (m_stopping) {
        m_outcome = Outcome::Cancelled;
    } else if (exitCode == 0 && m_error.isEmpty()) {
        m_outcome = Outcome::Done;
    } else {
        m_outcome = Outcome::Failed;
        if (m_error.isEmpty()) {
            const QString tail = QString::fromUtf8(m_errors.right(400)).trimmed();
            m_error = tail.isEmpty() ? QStringLiteral("бенчмарк завершился с кодом %1").arg(exitCode)
                                     : tail;
            m_log << QStringLiteral("Ошибка: %1").arg(m_error);
        }
    }
    emit changed();
    emit finished();
}
