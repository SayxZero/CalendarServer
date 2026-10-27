/*
 * CalendarServerBench - the benchmark of the server, installed next to it.
 *
 *   tune     measures the machine and picks the load settings for it: the
 *            admin panel runs it from its page "Многопоточность" (--json);
 *   probe    only the machine: processor, memory, disk;
 *   prepare  a base of made-up accounts;
 *   run      one run of the server on such a base, a row of Markdown -
 *            what tests\bench-all.ps1 is made of.
 *
 * Every server it starts listens on 127.0.0.1 and works on a copy of the
 * made-up base in a folder of its own: the real base is never touched.
 */

#include "basemaker.h"
#include "config.h"
#include "loadrun.h"
#include "probe.h"
#include "reporter.h"
#include "runtimefile.h"
#include "tuner.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include <atomic>
#include <csignal>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

std::atomic<bool> g_cancel { false };

void requestCancel(int)
{
    g_cancel = true;
}

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QString number(double value, int decimals = 1)
{
    return QString::number(value, 'f', decimals);
}

/*! The settings of a run from the options of the command line, over @p base. */
ServerSettings settingsFrom(const QCommandLineParser &args, ServerSettings base)
{
    const auto take = [&](const char *name, int *field) {
        if (args.isSet(QString::fromLatin1(name)))
            *field = args.value(QString::fromLatin1(name)).toInt();
    };
    take("threads", &base.threads);
    take("group-commit", &base.groupCommit);
    take("max-connections", &base.maxConnections);
    take("buffer-mb", &base.bufferMb);
    take("max-body-mb", &base.maxBodyMb);
    if (args.isSet(QStringLiteral("dispatch")))
        base.dispatch = args.value(QStringLiteral("dispatch"));
    if (args.isSet(QStringLiteral("sync-mode")))
        base.syncMode = args.value(QStringLiteral("sync-mode"));
    return base;
}

ServerSettings settingsOf(const ServerConfig &config)
{
    ServerSettings settings;
    settings.threads = config.threads;
    settings.dispatch = config.dispatch;
    settings.groupCommit = config.groupCommit;
    settings.syncMode = config.syncMode;
    settings.maxConnections = config.maxConnections;
    settings.bufferMb = config.bufferMb;
    settings.maxQueueWaitSeconds = config.maxQueueWaitSeconds;
    settings.maxBodyMb = config.maxBodyMb;
    return settings;
}

int runOne(const QCommandLineParser &args)
{
    LoadOptions options;
    options.serverExe = args.isSet(QStringLiteral("server")) ? args.value(QStringLiteral("server"))
                                                              : defaultServerExe();
    options.baseDir = args.value(QStringLiteral("base"));
    options.workDir = args.isSet(QStringLiteral("work"))
                          ? args.value(QStringLiteral("work"))
                          : QDir(options.baseDir).absoluteFilePath(QStringLiteral("../runs"));
    options.scenario = args.value(QStringLiteral("scenario"));
    options.clients = args.value(QStringLiteral("clients")).toInt();
    options.seconds = args.value(QStringLiteral("seconds")).toInt();
    options.settings = settingsFrom(args, ServerSettings());
    options.limits.affinity = args.value(QStringLiteral("affinity")).toInt();
    options.limits.memoryMb = args.value(QStringLiteral("memory-mb")).toInt();
    options.limits.cpuRate = args.value(QStringLiteral("cpu-rate")).toInt();
    options.keep = args.isSet(QStringLiteral("keep"));

    const LoadResult result = runLoad(options, &g_cancel);
    if (!result.started) {
        out() << "| " << args.value(QStringLiteral("label")) << " | " << options.scenario
              << " | не запустился: " << result.error << " |" << Qt::endl;
        return 1;
    }
    const QString row =
        QStringLiteral("| %1 | %2 | %3 | %4 | %5 | %6 | %7 | %8 | %9 | %10 | %11 | %12 | %13 | %14 |")
            .arg(args.value(QStringLiteral("label")), options.scenario)
            .arg(number(result.throughput, 0))
            .arg(number(result.p50), number(result.p95), number(result.p99))
            .arg(options.scenario == QLatin1String("skewed")
                     ? number(result.lightP50) + QStringLiteral(" / ") + number(result.lightP99)
                     : QStringLiteral("—"))
            .arg(result.refused)
            .arg(result.errors)
            .arg(result.cpuPercent >= 0 ? number(result.cpuPercent, 0) + QStringLiteral(" %") : QStringLiteral("—"),
                 result.peakMemoryMb >= 0 ? number(result.peakMemoryMb, 0) + QStringLiteral(" МБ") : QStringLiteral("—"),
                 result.threadSpread >= 0 ? number(result.threadSpread) + QStringLiteral(" %") : QStringLiteral("—"),
                 result.writesPerCommit > 0 ? number(result.writesPerCommit) : QStringLiteral("—"))
            .arg(result.died ? QStringLiteral("упал") : result.alive ? QStringLiteral("да")
                                                                     : QStringLiteral("не отвечает"));
    out() << row << Qt::endl;
    if (args.isSet(QStringLiteral("out"))) {
        QFile file(args.value(QStringLiteral("out")));
        if (file.open(QIODevice::WriteOnly | QIODevice::Append))
            file.write(row.toUtf8() + '\n');
    }
    if (!result.error.isEmpty())
        out() << "    " << result.error << Qt::endl;
    if (options.keep)
        out() << "kept " << result.keptFolder << Qt::endl;
    return result.died ? 2 : 0;
}

int tune(const QCommandLineParser &args)
{
    Reporter reporter(args.isSet(QStringLiteral("json")));
    const auto fail = [&](const QString &message, int code) {
        reporter.event(QStringLiteral("error"), QJsonObject { { QStringLiteral("message"), message } },
                       QStringLiteral("ОШИБКА: %1").arg(message));
        return code;
    };

    // The settings the server of this base runs with, and where its base is.
    ServerConfig config;
    QString error;
    if (args.isSet(QStringLiteral("config")) && !config.loadIni(args.value(QStringLiteral("config")), &error))
        return fail(error, 2);
    if (args.isSet(QStringLiteral("data-dir")))
        config.dataDir = args.value(QStringLiteral("data-dir"));
    const QString dataDir = config.resolvedDataDir();
    // Measuring next to a working server would measure both, and slow it down.
    const RuntimeInfo running = RuntimeFile::read(dataDir);
    if (running.running && !args.isSet(QStringLiteral("force"))) {
        return fail(QStringLiteral("сервер этой папки работает (процесс %1, порт %2): остановите его — "
                                   "замеры заняли бы его процессор и диск")
                        .arg(running.pid)
                        .arg(running.port),
                    3);
    }

    Tuner::Options options;
    options.serverExe = args.isSet(QStringLiteral("server")) ? args.value(QStringLiteral("server"))
                                                              : defaultServerExe();
    if (!QFile::exists(options.serverExe))
        return fail(QStringLiteral("нет сервера %1").arg(QDir::toNativeSeparators(options.serverExe)), 2);
    options.workDir = args.isSet(QStringLiteral("work"))
                          ? args.value(QStringLiteral("work"))
                          : QDir(dataDir).filePath(QStringLiteral("benchmark"));
    options.current = settingsOf(config);
    options.full = args.isSet(QStringLiteral("full"));
    if (args.isSet(QStringLiteral("clients")))
        options.clients = args.value(QStringLiteral("clients")).toInt();
    if (args.isSet(QStringLiteral("seconds")))
        options.seconds = args.value(QStringLiteral("seconds")).toInt();

    Tuner tuner(options);
    return tuner.run(reporter, &g_cancel);
}

int probe(const QCommandLineParser &args)
{
    Reporter reporter(args.isSet(QStringLiteral("json")));
    const QString folder = args.isSet(QStringLiteral("work")) ? args.value(QStringLiteral("work"))
                                                              : QDir::currentPath();
    const SystemInfo info = probeSystem(folder, &g_cancel, [&](double fraction, const QString &message) {
        reporter.progress(100 * fraction, message);
    });
    reporter.event(QStringLiteral("system"), info.toJson(), info.describe().join(QLatin1Char('\n')));
    return g_cancel ? 2 : 0;
}

int prepare(const QCommandLineParser &args)
{
    Reporter reporter(args.isSet(QStringLiteral("json")));
    BaseRequest request;
    request.dir = args.value(QStringLiteral("dir"));
    request.accounts = args.value(QStringLiteral("accounts")).toInt();
    request.records = args.value(QStringLiteral("records")).toInt();
    if (baseIsReady(request)) {
        reporter.event(QStringLiteral("done"), QJsonObject(), QStringLiteral("База уже готова"));
        return 0;
    }
    QString error;
    const bool made = prepareBase(request, &g_cancel, [&](double fraction, const QString &message) {
        reporter.progress(100 * fraction, message);
    }, &error);
    if (!made) {
        reporter.event(QStringLiteral("error"), QJsonObject { { QStringLiteral("message"), error } },
                       QStringLiteral("ОШИБКА: %1").arg(error));
        return 1;
    }
    reporter.event(QStringLiteral("done"), QJsonObject(),
                   QStringLiteral("База готова: %1 аккаунтов по %2 записей")
                       .arg(request.accounts)
                       .arg(request.records));
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("CalendarServerBench"));
    QCoreApplication::setApplicationVersion(QStringLiteral(SERVER_VERSION));
#ifdef Q_OS_WIN
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::signal(SIGINT, requestCancel);
    std::signal(SIGTERM, requestCancel);

    QCommandLineParser args;
    args.setApplicationDescription(QStringLiteral("Бенчмарк сервера Космос календаря: tune, probe, prepare, run"));
    args.addPositionalArgument(QStringLiteral("command"), QStringLiteral("tune, probe, prepare или run"));
    const QList<QCommandLineOption> options = {
        { QStringLiteral("json"), QStringLiteral("ход работы строками JSON (для админ-панели)") },
        { QStringLiteral("config"), QStringLiteral("server.ini сервера, чьи настройки подбирать"), QStringLiteral("file") },
        { QStringLiteral("data-dir"), QStringLiteral("папка данных этого сервера"), QStringLiteral("dir") },
        { QStringLiteral("work"), QStringLiteral("папка замеров (по умолчанию benchmark в папке данных)"),
          QStringLiteral("dir") },
        { QStringLiteral("server"), QStringLiteral("CalendarServer, по умолчанию рядом с бенчмарком"),
          QStringLiteral("exe") },
        { QStringLiteral("full"), QStringLiteral("подробный подбор: дольше и точнее") },
        { QStringLiteral("force"), QStringLiteral("мерить и при работающем сервере") },
        { QStringLiteral("dir"), QStringLiteral("prepare: папка базы"), QStringLiteral("dir") },
        { QStringLiteral("accounts"), QStringLiteral("prepare: аккаунтов"), QStringLiteral("n"), QStringLiteral("200") },
        { QStringLiteral("records"), QStringLiteral("prepare: записей у аккаунта"), QStringLiteral("n"),
          QStringLiteral("500") },
        { QStringLiteral("base"), QStringLiteral("run: подготовленная база"), QStringLiteral("dir") },
        { QStringLiteral("scenario"), QStringLiteral("run: pull, push, mixed, skewed, bulk"), QStringLiteral("name"),
          QStringLiteral("mixed") },
        { QStringLiteral("clients"), QStringLiteral("клиентов сразу"), QStringLiteral("n"), QStringLiteral("64") },
        { QStringLiteral("seconds"), QStringLiteral("секунд замера"), QStringLiteral("n"), QStringLiteral("20") },
        { QStringLiteral("threads"), QStringLiteral("run: потоков сервера"), QStringLiteral("n") },
        { QStringLiteral("dispatch"), QStringLiteral("run: очередь сервера"), QStringLiteral("name") },
        { QStringLiteral("group-commit"), QStringLiteral("run: запросов за сброс на диск"), QStringLiteral("n") },
        { QStringLiteral("sync-mode"), QStringLiteral("run: full или normal"), QStringLiteral("name") },
        { QStringLiteral("max-body-mb"), QStringLiteral("run: предел тела запроса"), QStringLiteral("n") },
        { QStringLiteral("buffer-mb"), QStringLiteral("run: память под запросы"), QStringLiteral("n") },
        { QStringLiteral("max-connections"), QStringLiteral("run: соединений сразу"), QStringLiteral("n") },
        { QStringLiteral("affinity"), QStringLiteral("run: только первые N ядер"), QStringLiteral("n"),
          QStringLiteral("0") },
        { QStringLiteral("memory-mb"), QStringLiteral("run: не больше N МБ памяти"), QStringLiteral("n"),
          QStringLiteral("0") },
        { QStringLiteral("cpu-rate"), QStringLiteral("run: не больше N % процессора"), QStringLiteral("n"),
          QStringLiteral("0") },
        { QStringLiteral("label"), QStringLiteral("run: подпись строки"), QStringLiteral("text"), QStringLiteral("run") },
        { QStringLiteral("out"), QStringLiteral("run: дописать строку в файл"), QStringLiteral("file") },
        { QStringLiteral("keep"), QStringLiteral("run: оставить папку прогона") },
    };
    args.addOptions(options);
    args.addHelpOption();
    args.addVersionOption();
    if (!args.parse(QCoreApplication::arguments())) {
        out() << args.errorText() << Qt::endl;
        return 2;
    }
    if (args.isSet(QStringLiteral("help"))) {
        out() << args.helpText() << Qt::flush;
        return 0;
    }
    if (args.isSet(QStringLiteral("version"))) {
        out() << "CalendarServerBench " << SERVER_VERSION << Qt::endl;
        return 0;
    }

    const QString command = args.positionalArguments().value(0);
    if (command == QLatin1String("tune"))
        return tune(args);
    if (command == QLatin1String("probe"))
        return probe(args);
    if (command == QLatin1String("prepare"))
        return prepare(args);
    if (command == QLatin1String("run"))
        return runOne(args);
    out() << args.helpText() << Qt::flush;
    return 2;
}
