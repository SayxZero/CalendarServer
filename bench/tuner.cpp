#include "tuner.h"

#include "basemaker.h"
#include "reporter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace {

//! How far below the fastest batch size a run still counts as just as fast.
constexpr double kCommitTolerance = 0.15;
//! The same for threads: on an SSD 4, 8 and 16 differed by 7 % at most,
//! while the p99 of 16 was three times that of 4.
constexpr double kThreadTolerance = 0.10;

QString ms(double value)
{
    return QLocale(QLocale::Russian).toString(value, 'f', value < 10 ? 1 : 0) + QStringLiteral(" мс");
}

QString times(double value)
{
    return QLocale(QLocale::Russian).toString(value, 'f', 1);
}

QString summary(const LoadResult &result)
{
    if (!result.started)
        return QStringLiteral("не прошёл: %1").arg(result.error);
    QString text = QStringLiteral("%1 запросов/с, p50 %2, p95 %3")
                       .arg(qRound(result.throughput))
                       .arg(ms(result.p50), ms(result.p95));
    if (result.refused > 0)
        text += QStringLiteral(", отказов 503 — %1").arg(result.refused);
    if (result.errors > 0)
        text += QStringLiteral(", ошибок %1").arg(result.errors);
    if (result.died)
        text += QStringLiteral(", сервер упал");
    else if (!result.alive)
        text += QStringLiteral(", сервер не отвечает после прогона");
    return text;
}

} // namespace

QJsonObject Tuner::Step::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("stage"), stage);
    object.insert(QStringLiteral("label"), label);
    object.insert(QStringLiteral("scenario"), scenario);
    object.insert(QStringLiteral("settings"), settings.toJson());
    object.insert(QStringLiteral("result"), result.toJson());
    return object;
}

Tuner::Tuner(const Options &options)
    : m_options(options)
{
}

QList<int> Tuner::threadCandidates(int cores)
{
    cores = qMax(1, cores);
    QList<int> candidates;
    for (const int count : { cores / 2, cores, cores * 2, cores * 4 }) {
        const int bounded = qBound(2, count, 64);
        if (!candidates.contains(bounded))
            candidates << bounded;
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates;
}

QList<int> Tuner::commitCandidates()
{
    return { 0, 8, 64, 256 };
}

QList<int> Tuner::costsAround(const QList<int> &candidates, int current)
{
    QList<int> costs;
    for (const int candidate : candidates)
        costs << (candidate == current ? 0 : 1 + qAbs(candidates.indexOf(candidate) - candidates.indexOf(current)));
    // A current value that is no candidate: the smaller the cheaper.
    if (!candidates.contains(current)) {
        costs.clear();
        for (int i = 0; i < candidates.size(); ++i)
            costs << 1 + i;
    }
    return costs;
}

int Tuner::pickBest(const QList<LoadResult> &results, const QList<int> &costs, double tolerance)
{
    double fastest = -1;
    for (const LoadResult &result : results) {
        if (result.usable())
            fastest = qMax(fastest, result.throughput);
    }
    if (fastest <= 0)
        return -1;
    double lowestP95 = -1;
    for (const LoadResult &result : results) {
        if (result.usable() && result.throughput >= (1 - tolerance) * fastest)
            lowestP95 = lowestP95 < 0 ? result.p95 : qMin(lowestP95, result.p95);
    }
    int best = -1;
    for (int i = 0; i < results.size(); ++i) {
        const LoadResult &result = results.at(i);
        if (!result.usable() || result.throughput < (1 - tolerance) * fastest
            || result.p95 > 1.3 * lowestP95 + 0.5) {
            continue;
        }
        if (best < 0 || costs.value(i) < costs.value(best)
            || (costs.value(i) == costs.value(best) && result.throughput > results.at(best).throughput)) {
            best = i;
        }
    }
    return best;
}

int Tuner::pickFairer(const QList<LoadResult> &results, int preferred)
{
    double fastest = -1;
    for (const LoadResult &result : results) {
        if (result.usable())
            fastest = qMax(fastest, result.throughput);
    }
    if (fastest <= 0)
        return -1;
    int best = -1;
    for (int i = 0; i < results.size(); ++i) {
        const LoadResult &result = results.at(i);
        if (!result.usable() || result.throughput < 0.8 * fastest)
            continue;
        if (best < 0) {
            best = i;
            continue;
        }
        const double mine = result.lightP99;
        const double theirs = results.at(best).lightP99;
        // Within a tenth of each other it is a tie, and the preferred one stays.
        if (mine < 0.9 * theirs || (mine <= 1.1 * theirs && i == preferred))
            best = i;
    }
    return best;
}

LoadResult Tuner::measure(Reporter &reporter, const std::atomic<bool> *cancel, const QString &stage,
                          const QString &label, const QString &scenario,
                          const ServerSettings &settings)
{
    LoadOptions options;
    options.serverExe = m_options.serverExe;
    options.baseDir = m_baseDir;
    options.workDir = QDir(m_options.workDir).filePath(QStringLiteral("runs"));
    options.scenario = scenario;
    options.clients = m_options.clients > 0 ? m_options.clients : (m_options.full ? 64 : 48);
    options.seconds = m_options.seconds > 0 ? m_options.seconds : (m_options.full ? 20 : 8);
    options.settings = settings;

    const double start = m_unitsBefore + m_runsDone;
    const int number = m_runsDone + 1;
    reporter.progress(100 * start / m_unitsTotal,
                      QStringLiteral("Прогон %1 из %2: %3").arg(number).arg(m_runsTotal).arg(label));
    const LoadResult result = runLoad(options, cancel, [&](double fraction, const QString &message) {
        reporter.progress(100 * (start + fraction) / m_unitsTotal,
                          QStringLiteral("Прогон %1 из %2: %3 — %4")
                              .arg(number)
                              .arg(m_runsTotal)
                              .arg(label, message));
    });
    ++m_runsDone;

    Step step;
    step.stage = stage;
    step.label = label;
    step.scenario = scenario;
    step.settings = settings;
    step.result = result;
    m_steps << step;
    reporter.event(QStringLiteral("run"), step.toJson(),
                   QStringLiteral("    %1: %2").arg(label, summary(result)));
    return result;
}

int Tuner::run(Reporter &reporter, const std::atomic<bool> *cancel)
{
    const auto cancelled = [cancel] { return cancel && cancel->load(); };
    const auto stopHere = [&] {
        reporter.event(QStringLiteral("cancelled"), QJsonObject(), QStringLiteral("Остановлено."));
        return 2;
    };
    QDir().mkpath(m_options.workDir);

    // Folders of runs a stopped benchmark left behind.
    QDir runs(QDir(m_options.workDir).filePath(QStringLiteral("runs")));
    for (const QString &left : runs.entryList({ QStringLiteral("run-*") }, QDir::Dirs | QDir::NoDotAndDotDot))
        QDir(runs.filePath(left)).removeRecursively();

    BaseRequest base;
    base.dir = QDir(m_options.workDir).filePath(QStringLiteral("base"));
    base.accounts = m_options.full ? 200 : 100;
    base.records = m_options.full ? 500 : 300;
    m_baseDir = base.dir;
    const bool needBase = !baseIsReady(base);

    const int cores = qMax(1, QThread::idealThreadCount());
    const QList<int> threads = threadCandidates(cores);
    const QList<int> commits = commitCandidates();
    const QList<QString> queues = { QStringLiteral("fair"), QStringLiteral("fifo") };
    m_runsTotal = int(threads.size() + commits.size() + queues.size()) + 1 + 1 + 2;
    m_unitsBefore = 1 + (needBase ? 3 : 0);
    m_unitsTotal = m_unitsBefore + m_runsTotal;
    const int seconds = m_options.seconds > 0 ? m_options.seconds : (m_options.full ? 20 : 8);
    const int estimate = 20 + (needBase ? base.accounts * base.records / 1500 : 0)
                         + m_runsTotal * (seconds + 7);
    const QList<QPair<QString, QString>> stages = {
        { QStringLiteral("probe"), QStringLiteral("Проверка машины") },
        { QStringLiteral("base"), QStringLiteral("Тестовая база") },
        { QStringLiteral("threads"), QStringLiteral("Число потоков") },
        { QStringLiteral("commit"), QStringLiteral("Групповая запись") },
        { QStringLiteral("queue"), QStringLiteral("Очередь к потокам") },
        { QStringLiteral("sync"), QStringLiteral("Запись без fsync") },
        { QStringLiteral("memory"), QStringLiteral("Большие запросы") },
        { QStringLiteral("final"), QStringLiteral("Итог") },
    };
    reporter.plan(stages, m_runsTotal, estimate);
    int stageIndex = 0;
    const auto enter = [&](int index) {
        stageIndex = index;
        reporter.stage(index, stages.at(index).first, stages.at(index).second);
    };

    // ---- the machine -------------------------------------------------------------
    enter(0);
    m_system = probeSystem(m_options.workDir, cancel, [&](double fraction, const QString &message) {
        reporter.progress(100 * fraction / m_unitsTotal, QStringLiteral("Проверка машины: %1").arg(message));
    });
    if (cancelled())
        return stopHere();
    reporter.event(QStringLiteral("system"), m_system.toJson(),
                   QStringLiteral("    ") + m_system.describe().join(QStringLiteral("\n    ")));
    if (m_system.freeBytes >= 0 && m_system.freeBytes < 512ll * 1024 * 1024) {
        reporter.event(QStringLiteral("error"),
                       QJsonObject { { QStringLiteral("message"),
                                       QStringLiteral("На диске меньше 512 МБ свободного места") } },
                       QStringLiteral("ОШИБКА: на диске меньше 512 МБ свободного места"));
        return 1;
    }

    // ---- the base ------------------------------------------------------------------
    enter(1);
    if (needBase) {
        QString error;
        const bool made = prepareBase(base, cancel, [&](double fraction, const QString &message) {
            reporter.progress(100 * (1 + 3 * fraction) / m_unitsTotal,
                              QStringLiteral("Тестовая база: %1").arg(message));
        }, &error);
        if (cancelled())
            return stopHere();
        if (!made) {
            reporter.event(QStringLiteral("error"), QJsonObject { { QStringLiteral("message"), error } },
                           QStringLiteral("ОШИБКА: тестовая база: %1").arg(error));
            return 1;
        }
    } else {
        reporter.progress(100 * m_unitsBefore / m_unitsTotal,
                          QStringLiteral("Тестовая база готова с прошлого раза"));
    }

    const ServerSettings current = m_options.current;
    ServerSettings chosen = current;
    QStringList notes;
    const int currentThreads = current.threads > 0 ? current.threads : qMax(2, cores);

    // ---- threads ---------------------------------------------------------------------
    enter(2);
    QList<LoadResult> threadResults;
    for (const int count : threads) {
        ServerSettings settings = chosen;
        settings.threads = count;
        threadResults << measure(reporter, cancel, QStringLiteral("threads"),
                                 QStringLiteral("потоков %1").arg(count), QStringLiteral("mixed"),
                                 settings);
        if (cancelled())
            return stopHere();
    }
    const int bestThreads =
        pickBest(threadResults, costsAround(threads, currentThreads), kThreadTolerance);
    if (bestThreads >= 0) {
        const int count = threads.at(bestThreads);
        // The number the server picks by itself stays "auto" when it is the best.
        chosen.threads = current.threads == 0 && count == qMax(2, cores) ? 0 : count;
        if (count > cores) {
            notes << QStringLiteral("Потоков больше, чем ядер (%1 при %2 ядрах): запросы ждут диск, и "
                                    "лишние потоки тем временем принимают следующие.")
                         .arg(count)
                         .arg(cores);
        }
    } else {
        notes << QStringLiteral("Ни один прогон с разным числом потоков не прошёл без ошибок — число "
                                "потоков оставлено как было.");
    }

    // ---- group commit ------------------------------------------------------------------
    enter(3);
    QList<LoadResult> commitResults;
    for (const int batch : commits) {
        ServerSettings settings = chosen;
        settings.groupCommit = batch;
        commitResults << measure(reporter, cancel, QStringLiteral("commit"),
                                 batch > 0 ? QStringLiteral("запись по %1").arg(batch)
                                           : QStringLiteral("запись по одной"),
                                 QStringLiteral("push"), settings);
        if (cancelled())
            return stopHere();
    }
    // Batches of 8, 64 and 256 differ by noise alone (two runs in a row: 64
    // and 256 by turns, ±10 %): only a clear gain is worth a change.
    const int bestCommit =
        pickBest(commitResults, costsAround(commits, current.groupCommit), kCommitTolerance);
    LoadResult fullSyncPush;
    if (bestCommit >= 0) {
        chosen.groupCommit = commits.at(bestCommit);
        fullSyncPush = commitResults.at(bestCommit);
        const LoadResult &off = commitResults.at(0);
        if (chosen.groupCommit > 0 && off.usable() && off.throughput > 0
            && fullSyncPush.throughput > 1.5 * off.throughput) {
            notes << QStringLiteral("Групповая запись ускорила правки в %1 раза: %2 запросов/с против %3 "
                                    "без неё.")
                         .arg(times(fullSyncPush.throughput / off.throughput))
                         .arg(qRound(fullSyncPush.throughput))
                         .arg(qRound(off.throughput));
        }
    }
    if (m_system.slowDisk()) {
        notes << QStringLiteral("Диск сбрасывает данные медленно (запись с fsync — %1): правки упираются в "
                                "него, а не в процессор.")
                     .arg(ms(m_system.commitMs));
    }

    // ---- the queue ------------------------------------------------------------------------
    enter(4);
    QList<LoadResult> queueResults;
    for (const QString &mode : queues) {
        ServerSettings settings = chosen;
        settings.dispatch = mode;
        queueResults << measure(reporter, cancel, QStringLiteral("queue"), QStringLiteral("очередь %1").arg(mode),
                                QStringLiteral("skewed"), settings);
        if (cancelled())
            return stopHere();
    }
    const int bestQueue = pickFairer(queueResults, qMax(0, int(queues.indexOf(current.dispatch))));
    if (bestQueue >= 0)
        chosen.dispatch = queues.at(bestQueue);

    // ---- without fsync: what it would give ------------------------------------------------
    enter(5);
    {
        ServerSettings settings = chosen;
        settings.syncMode = QStringLiteral("normal");
        const LoadResult normal = measure(reporter, cancel, QStringLiteral("sync"),
                                          QStringLiteral("запись без fsync"), QStringLiteral("push"),
                                          settings);
        if (cancelled())
            return stopHere();
        if (normal.usable() && fullSyncPush.usable() && fullSyncPush.throughput > 0) {
            const double gain = normal.throughput / fullSyncPush.throughput;
            if (gain > 1.3 && current.syncMode != QLatin1String("normal")) {
                notes << QStringLiteral("Без fsync каждого сброса правки шли бы в %1 раза быстрее, но при "
                                        "пропадании питания пропадут обмены последних секунд, а устройства "
                                        "будут считать их принятыми. Включайте sync_mode = normal, только "
                                        "если у сервера есть источник бесперебойного питания.")
                             .arg(times(gain));
            } else if (gain <= 1.3) {
                notes << QStringLiteral("Запись без fsync почти ничего не даёт на этом диске (×%1) — "
                                        "оставьте sync_mode = full.")
                             .arg(times(gain));
            }
        }
    }

    // ---- big bodies -----------------------------------------------------------------------
    enter(6);
    {
        const LoadResult bulk = measure(reporter, cancel, QStringLiteral("memory"),
                                        QStringLiteral("большие запросы"), QStringLiteral("bulk"),
                                        chosen);
        if (cancelled())
            return stopHere();
        if (!bulk.usable()) {
            notes << QStringLiteral("Сервер не выдержал больших запросов (%1): уменьшите max_body_mb или "
                                    "buffer_mb.")
                         .arg(summary(bulk));
        } else if (bulk.peakMemoryMb > 0) {
            notes << QStringLiteral("Большие запросы (по 1,7 МБ от %1 клиентов) выдержаны: пик памяти "
                                    "сервера %2 МБ.")
                         .arg(m_options.clients > 0 ? m_options.clients : (m_options.full ? 64 : 48))
                         .arg(qRound(bulk.peakMemoryMb));
        }
    }

    // ---- what it gives ----------------------------------------------------------------------
    enter(7);
    const LoadResult before = measure(reporter, cancel, QStringLiteral("final"),
                                      QStringLiteral("как сейчас"), QStringLiteral("mixed"), current);
    if (cancelled())
        return stopHere();
    const LoadResult after = measure(reporter, cancel, QStringLiteral("final"),
                                     QStringLiteral("рекомендованные"), QStringLiteral("mixed"), chosen);
    if (cancelled())
        return stopHere();
    double gain = 0;
    if (before.usable() && after.usable() && before.throughput > 0)
        gain = 100 * (after.throughput / before.throughput - 1);
    if (chosen == current)
        notes.prepend(QStringLiteral("Текущие настройки уже лучшие из проверенных."));
    else if (before.usable() && after.usable())
        notes.prepend(QStringLiteral("Рекомендованные настройки: %1 запросов/с против %2 сейчас (%3%4 %).")
                          .arg(qRound(after.throughput))
                          .arg(qRound(before.throughput))
                          .arg(gain >= 0 ? QStringLiteral("+") : QString())
                          .arg(qRound(gain)));

    m_recommendation = QJsonObject();
    m_recommendation.insert(QStringLiteral("settings"), chosen.toJson());
    m_recommendation.insert(QStringLiteral("current"), current.toJson());
    m_recommendation.insert(QStringLiteral("gainPercent"), gain);
    m_recommendation.insert(QStringLiteral("notes"), QJsonArray::fromStringList(notes));

    QJsonObject document;
    document.insert(QStringLiteral("finishedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    document.insert(QStringLiteral("mode"), m_options.full ? QStringLiteral("full") : QStringLiteral("quick"));
    document.insert(QStringLiteral("seconds"), reporter.elapsedSeconds());
    document.insert(QStringLiteral("system"), m_system.toJson());
    QJsonArray steps;
    for (const Step &step : m_steps)
        steps.append(step.toJson());
    document.insert(QStringLiteral("steps"), steps);
    document.insert(QStringLiteral("recommendation"), m_recommendation);
    const QString resultPath = QDir(m_options.workDir).filePath(QStringLiteral("result.json"));
    QFile file(resultPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(document).toJson());

    QStringList text;
    text << QStringLiteral("=== Рекомендация: %1").arg(chosen.describe());
    for (const QString &note : notes)
        text << QStringLiteral("    ") + note;
    reporter.event(QStringLiteral("recommendation"), m_recommendation, text.join(QLatin1Char('\n')));
    reporter.progress(100, QStringLiteral("Готово"));
    reporter.event(QStringLiteral("done"), QJsonObject { { QStringLiteral("result"), resultPath } },
                   QStringLiteral("Готово за %1 мин. Результат: %2")
                       .arg(qMax(1, qRound(reporter.elapsedSeconds() / 60)))
                       .arg(QDir::toNativeSeparators(resultPath)));
    Q_UNUSED(stageIndex);
    return 0;
}
