#pragma once

#include "loadrun.h"
#include "probe.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>

class Reporter;

/*!
 * The tuning of the load settings for this machine: it measures the machine
 * (probeSystem()), prepares a base of made-up accounts next to the real one
 * (on the same disk: the disk is half of what is measured), and runs the
 * real server on copies of it with one setting changed at a time:
 *
 *   threads  - mixed load, 4 thread counts around the number of cores;
 *   commit   - pushes, group commit off, 8, 64, 256 operations a commit;
 *   queue    - one noisy account against light clients, fair against fifo;
 *   sync     - pushes without fsync of each commit: what it would give
 *              (never recommended by itself: it is the admin's choice);
 *   memory   - big bodies with the chosen settings: does the server hold;
 *   final    - mixed load, the current settings against the chosen ones.
 *
 * Each stage keeps what the earlier ones chose. What it recommends and why
 * goes to the reporter and into result.json in the work folder.
 */
class Tuner
{
public:
    struct Options
    {
        QString serverExe;
        QString workDir;
        ServerSettings current;   //!< what the server of this base runs with now
        bool full = false;        //!< longer runs, a bigger base: about three times longer
        int clients = 0;          //!< 0 - by the mode
        int seconds = 0;          //!< 0 - by the mode
    };

    /*! One run of the plan and how it went. */
    struct Step
    {
        QString stage;
        QString label;
        QString scenario;
        ServerSettings settings;
        LoadResult result;

        QJsonObject toJson() const;
    };

    explicit Tuner(const Options &options);

    /*! The whole tuning; 0 when done, 1 on failure, 2 when cancelled. */
    int run(Reporter &reporter, const std::atomic<bool> *cancel);

    // ---- the decisions, apart from the measuring (tst_bench) ----------------
    /*! Half the cores, the cores, twice and four times: 2 to 64, without repeats. */
    static QList<int> threadCandidates(int cores);
    static QList<int> commitCandidates();

    /*!
     * The best of @p results: among the usable ones within @p tolerance of
     * the highest throughput and with a p95 no more than 30 % above the
     * lowest of them, the one of the lowest @p costs (the current value
     * costs 0: what is not clearly better is not changed), then the faster.
     * -1 when none is usable.
     */
    static int pickBest(const QList<LoadResult> &results, const QList<int> &costs,
                        double tolerance = 0.05);

    /*!
     * The queue: the lower p99 of the light clients, unless that costs more
     * than a fifth of the throughput; @p preferred wins a tie.
     */
    static int pickFairer(const QList<LoadResult> &results, int preferred);

    /*! The cost of a candidate: 0 for @p current, otherwise its distance from it. */
    static QList<int> costsAround(const QList<int> &candidates, int current);

    QJsonObject recommendation() const { return m_recommendation; }
    const QList<Step> &steps() const { return m_steps; }

private:
    LoadResult measure(Reporter &reporter, const std::atomic<bool> *cancel, const QString &stage,
                       const QString &label, const QString &scenario, const ServerSettings &settings);

    Options m_options;
    QString m_baseDir;
    int m_runsTotal = 0;
    int m_runsDone = 0;
    double m_unitsBefore = 0;   //!< probe and base, in runs
    double m_unitsTotal = 1;
    QList<Step> m_steps;
    QJsonObject m_recommendation;
    SystemInfo m_system;
};
