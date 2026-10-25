#include "service.h"

#ifdef Q_OS_WIN

#include <windows.h>

#include <atomic>
#include <cstdio>

namespace {

SERVICE_STATUS_HANDLE g_handle = nullptr;
SERVICE_STATUS g_status = {};
std::atomic_bool g_active { false };
std::atomic_bool g_stop { false };

int g_argc = 0;
char **g_argv = nullptr;
Service::Body g_body = nullptr;
int g_exitCode = 0;

void setState(DWORD state, DWORD waitHint = 0)
{
    static DWORD checkPoint = 1;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWaitHint = waitHint;
    // A pending state has to show progress, or the manager gives up on it.
    const bool pending = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING;
    g_status.dwCheckPoint = pending ? checkPoint++ : 0;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING
                                      ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
                                      : 0;
    SetServiceStatus(g_handle, &g_status);
}

/*!
 * Runs on the thread of the dispatcher, not on the one of the server: it only
 * raises the flag, and the server's own timer ends the event loop.
 */
DWORD WINAPI control(DWORD code, DWORD, LPVOID, LPVOID)
{
    switch (code) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        setState(SERVICE_STOP_PENDING, 10000);
        g_stop = true;
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI serviceMain(DWORD, LPWSTR *)
{
    g_handle = RegisterServiceCtrlHandlerExW(L"CosmosCalendarServer", control, nullptr);
    if (!g_handle)
        return;
    g_active = true;
    setState(SERVICE_START_PENDING, 30000);

    g_exitCode = g_body(g_argc, g_argv);

    // A failure is reported as a code of the service's own; why it failed is
    // in server.log, which the admin panel shows next to the code.
    g_status.dwWin32ExitCode = g_exitCode == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
    g_status.dwServiceSpecificExitCode = DWORD(g_exitCode);
    setState(SERVICE_STOPPED);
}

} // namespace

int Service::run(int argc, char **argv, Body body)
{
    g_argc = argc;
    g_argv = argv;
    g_body = body;

    // The name is not checked for a service of its own process.
    wchar_t name[] = L"CosmosCalendarServer";
    const SERVICE_TABLE_ENTRYW table[] = { { name, serviceMain }, { nullptr, nullptr } };
    if (StartServiceCtrlDispatcherW(table))
        return g_exitCode;

    const DWORD error = GetLastError();
    const char *message = error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT
                              ? "С --service сервер запускает только диспетчер служб Windows; "
                                "в консоли он запускается без этого ключа.\n"
                              : "Диспетчер служб Windows не принял процесс.\n";
    SetConsoleOutputCP(CP_UTF8);
    std::fputs(message, stderr);
    return 2;
}

bool Service::active()
{
    return g_active;
}

void Service::reportRunning()
{
    if (g_active)
        setState(SERVICE_RUNNING);
}

bool Service::stopRequested()
{
    return g_stop;
}

#else

int Service::run(int argc, char **argv, Body body)
{
    return body(argc, argv);
}

bool Service::active()
{
    return false;
}

void Service::reportRunning()
{
}

bool Service::stopRequested()
{
    return false;
}

#endif
