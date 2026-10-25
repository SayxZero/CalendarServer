#include "servicecontrol.h"

#include "service.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#else
#include <QProcess>
#include <unistd.h>
#endif

QString ServiceControl::defaultName()
{
#ifdef Q_OS_WIN
    return Service::name();
#else
    return QStringLiteral("calendar-server");
#endif
}

ServiceControl::ServiceControl(const QString &name)
    : m_name(name)
{
}

bool ServiceControl::configOf(const QStringList &arguments, ServerConfig *config, QString *error)
{
    QCommandLineParser parser;
    const ServerOptions options;
    parser.addOptions(options.all());
    // parse() wants the program in front, as in argv.
    if (!parser.parse(QStringList { QStringLiteral("CalendarServer") } + arguments)) {
        if (error)
            *error = parser.errorText();
        return false;
    }
    return config->apply(parser, options, error);
}

ServiceControl::State ServiceControl::settle(int timeoutMs) const
{
    QElapsedTimer timer;
    timer.start();
    State state = status().state;
    while ((state == State::Starting || state == State::Stopping) && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
        QThread::msleep(100);
        state = status().state;
    }
    return state;
}

QString ServiceControl::stateText(State state)
{
    switch (state) {
    case State::Unknown:
        return QStringLiteral("неизвестно");
    case State::Missing:
        return QStringLiteral("служба не установлена");
    case State::Stopped:
        return QStringLiteral("остановлен");
    case State::Starting:
        return QStringLiteral("запускается");
    case State::Stopping:
        return QStringLiteral("останавливается");
    case State::Running:
        return QStringLiteral("работает");
    }
    return QString();
}

#ifdef Q_OS_WIN

namespace {

QString systemMessage(DWORD code)
{
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                            | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0,
                                        nullptr);
    QString text = length ? QString::fromWCharArray(buffer, int(length)).trimmed()
                          : QStringLiteral("ошибка %1").arg(code);
    if (buffer)
        LocalFree(buffer);
    return text;
}

/*! A handle of the service control manager or of a service, closed when it goes. */
class Handle
{
public:
    explicit Handle(SC_HANDLE handle = nullptr) : m_handle(handle) {}
    ~Handle()
    {
        if (m_handle)
            CloseServiceHandle(m_handle);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;

    operator SC_HANDLE() const { return m_handle; }
    explicit operator bool() const { return m_handle != nullptr; }

private:
    SC_HANDLE m_handle;
};

QDateTime processStart(DWORD pid)
{
    QDateTime result;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return result;
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        // FILETIME counts 100 ns steps from 1601.
        const quint64 ticks = (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        const qint64 msecs = qint64(ticks / 10000) - Q_INT64_C(11644473600000);
        result = QDateTime::fromMSecsSinceEpoch(msecs);
    }
    CloseHandle(process);
    return result;
}

} // namespace

ServiceControl::Status ServiceControl::status() const
{
    Status status;
    Handle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) {
        status.error = systemMessage(GetLastError());
        return status;
    }
    Handle service(OpenServiceW(manager, reinterpret_cast<LPCWSTR>(m_name.utf16()),
                                SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
    if (!service) {
        const DWORD code = GetLastError();
        if (code == ERROR_SERVICE_DOES_NOT_EXIST)
            status.state = State::Missing;
        else
            status.error = systemMessage(code);
        return status;
    }

    SERVICE_STATUS_PROCESS process = {};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&process),
                              sizeof process, &needed)) {
        status.error = systemMessage(GetLastError());
        return status;
    }
    switch (process.dwCurrentState) {
    case SERVICE_RUNNING:
        status.state = State::Running;
        break;
    case SERVICE_START_PENDING:
    case SERVICE_CONTINUE_PENDING:
        status.state = State::Starting;
        break;
    case SERVICE_STOP_PENDING:
    case SERVICE_PAUSE_PENDING:
        status.state = State::Stopping;
        break;
    default:
        status.state = State::Stopped;
        break;
    }
    status.pid = process.dwProcessId;
    if (status.pid)
        status.startedAt = processStart(process.dwProcessId);
    if (status.state == State::Stopped) {
        if (process.dwWin32ExitCode == ERROR_SERVICE_SPECIFIC_ERROR)
            status.exitCode = int(process.dwServiceSpecificExitCode);
        else if (process.dwWin32ExitCode != NO_ERROR)
            status.exitCode = int(process.dwWin32ExitCode);
    }

    QueryServiceConfigW(service, nullptr, 0, &needed);
    QByteArray buffer(int(needed), Qt::Uninitialized);
    auto *config = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buffer.data());
    if (needed && QueryServiceConfigW(service, config, needed, &needed))
        status.autoStart = config->dwStartType == SERVICE_AUTO_START;
    return status;
}

QStringList ServiceControl::arguments(QString *error) const
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QStringList();
    };
    Handle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager)
        return fail(systemMessage(GetLastError()));
    Handle service(OpenServiceW(manager, reinterpret_cast<LPCWSTR>(m_name.utf16()),
                                SERVICE_QUERY_CONFIG));
    if (!service)
        return fail(systemMessage(GetLastError()));

    DWORD needed = 0;
    QueryServiceConfigW(service, nullptr, 0, &needed);
    QByteArray buffer(int(needed), Qt::Uninitialized);
    auto *config = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buffer.data());
    if (!needed || !QueryServiceConfigW(service, config, needed, &needed))
        return fail(systemMessage(GetLastError()));

    // The image path is one command line, split by the rules of the C runtime.
    int count = 0;
    LPWSTR *argv = CommandLineToArgvW(config->lpBinaryPathName, &count);
    if (!argv)
        return fail(systemMessage(GetLastError()));
    QStringList result;
    for (int i = 1; i < count; ++i)
        result << QString::fromWCharArray(argv[i]);
    LocalFree(argv);
    return result;
}

bool ServiceControl::start(QString *error)
{
    Handle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    Handle service(manager ? OpenServiceW(manager, reinterpret_cast<LPCWSTR>(m_name.utf16()),
                                          SERVICE_START)
                           : nullptr);
    if (!service || !StartServiceW(service, 0, nullptr)) {
        const DWORD code = GetLastError();
        if (code == ERROR_SERVICE_ALREADY_RUNNING)
            return true;
        if (error)
            *error = systemMessage(code);
        return false;
    }
    return true;
}

bool ServiceControl::stop(QString *error)
{
    Handle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    Handle service(manager ? OpenServiceW(manager, reinterpret_cast<LPCWSTR>(m_name.utf16()),
                                          SERVICE_STOP)
                           : nullptr);
    SERVICE_STATUS status = {};
    if (!service || !ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        const DWORD code = GetLastError();
        if (code == ERROR_SERVICE_NOT_ACTIVE)
            return true;
        if (error)
            *error = systemMessage(code);
        return false;
    }
    return true;
}

bool ServiceControl::isElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

bool ServiceControl::relaunchElevated(const QStringList &arguments, bool *declined)
{
    if (declined)
        *declined = false;
    // Quoted the way CommandLineToArgvW reads them back: backslashes count
    // only in front of a quote, the closing one included ("D:\data\").
    QStringList quoted;
    for (const QString &argument : arguments) {
        QString text = QStringLiteral("\"");
        int slashes = 0;
        for (const QChar c : argument) {
            if (c == QLatin1Char('\\')) {
                ++slashes;
                continue;
            }
            text += QString(c == QLatin1Char('"') ? slashes * 2 + 1 : slashes, QLatin1Char('\\'));
            text += c;
            slashes = 0;
        }
        text += QString(slashes * 2, QLatin1Char('\\')) + QLatin1Char('"');
        quoted << text;
    }
    const QString program = QCoreApplication::applicationFilePath();
    const QString parameters = quoted.join(QLatin1Char(' '));

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof info;
    info.lpVerb = L"runas";
    info.lpFile = reinterpret_cast<LPCWSTR>(program.utf16());
    info.lpParameters = reinterpret_cast<LPCWSTR>(parameters.utf16());
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info))
        return true;
    if (declined)
        *declined = GetLastError() == ERROR_CANCELLED;
    return false;
}

#else

namespace {

/*! systemctl, answered in plain text; the panel only reads what it says. */
bool systemctl(const QStringList &arguments, QString *output, QString *error)
{
    QProcess process;
    process.start(QStringLiteral("systemctl"), arguments);
    if (!process.waitForFinished(15000)) {
        if (error)
            *error = process.errorString();
        return false;
    }
    if (output)
        *output = QString::fromUtf8(process.readAllStandardOutput());
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error)
            *error = QString::fromUtf8(process.readAllStandardError()).trimmed();
        return false;
    }
    return true;
}

QHash<QString, QString> properties(const QString &unit, QString *error)
{
    QHash<QString, QString> result;
    QString output;
    if (!systemctl({ QStringLiteral("show"), unit, QStringLiteral("--no-pager"),
                     QStringLiteral("-p"), QStringLiteral("LoadState"),
                     QStringLiteral("-p"), QStringLiteral("ActiveState"),
                     QStringLiteral("-p"), QStringLiteral("MainPID"),
                     QStringLiteral("-p"), QStringLiteral("ExecMainStartTimestamp"),
                     QStringLiteral("-p"), QStringLiteral("ExecMainStatus"),
                     QStringLiteral("-p"), QStringLiteral("UnitFileState"),
                     QStringLiteral("-p"), QStringLiteral("ExecStart") },
                   &output, error)) {
        return result;
    }
    for (const QString &line : output.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals > 0)
            result.insert(line.left(equals), line.mid(equals + 1));
    }
    return result;
}

} // namespace

ServiceControl::Status ServiceControl::status() const
{
    Status status;
    const QHash<QString, QString> values = properties(m_name + QStringLiteral(".service"), &status.error);
    if (values.isEmpty())
        return status;
    if (values.value(QStringLiteral("LoadState")) == QLatin1String("not-found")) {
        status.state = State::Missing;
        return status;
    }
    const QString active = values.value(QStringLiteral("ActiveState"));
    status.state = active == QLatin1String("active")         ? State::Running
                   : active == QLatin1String("activating")   ? State::Starting
                   : active == QLatin1String("deactivating") ? State::Stopping
                                                             : State::Stopped;
    status.pid = values.value(QStringLiteral("MainPID")).toLongLong();
    status.autoStart = values.value(QStringLiteral("UnitFileState")) == QLatin1String("enabled");
    if (status.state == State::Stopped)
        status.exitCode = values.value(QStringLiteral("ExecMainStatus")).toInt();
    return status;
}

QStringList ServiceControl::arguments(QString *error) const
{
    const QHash<QString, QString> values = properties(m_name + QStringLiteral(".service"), error);
    // ExecStart={ path=/usr/local/bin/CalendarServer ; argv[]=/usr/local/bin/CalendarServer -d … ; … }
    const QString exec = values.value(QStringLiteral("ExecStart"));
    const int from = exec.indexOf(QLatin1String("argv[]="));
    if (from < 0)
        return QStringList();
    const int to = exec.indexOf(QLatin1String(" ;"), from);
    QStringList result = QProcess::splitCommand(exec.mid(from + 7, to < 0 ? -1 : to - from - 7));
    if (!result.isEmpty())
        result.removeFirst();
    return result;
}

bool ServiceControl::start(QString *error)
{
    return systemctl({ QStringLiteral("start"), m_name }, nullptr, error);
}

bool ServiceControl::stop(QString *error)
{
    return systemctl({ QStringLiteral("stop"), m_name }, nullptr, error);
}

bool ServiceControl::isElevated()
{
    return geteuid() == 0;
}

bool ServiceControl::relaunchElevated(const QStringList &, bool *declined)
{
    if (declined)
        *declined = false;
    return false;
}

#endif
