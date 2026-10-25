#pragma once

#include <QString>

/*!
 * The server as a Windows service: the service control manager starts
 * "CalendarServer.exe --service ...", and this is the side that answers it.
 *
 * The server itself does not change: the whole of it runs in @p body on the
 * thread the manager gives the service, and a stop request ends its event loop
 * the way Ctrl+C does in a console. Elsewhere --service needs nothing of this
 * (systemd starts a plain process), and run() just calls @p body.
 */
namespace Service {

/*! The name the service is installed under: the MSI and the admin panel use it too. */
inline QString name() { return QStringLiteral("CosmosCalendarServer"); }

using Body = int (*)(int argc, char **argv);

/*!
 * Hands the process to the service control manager and runs @p body as the
 * service; returns its exit code once the service has stopped. When the
 * process was not started by the manager, prints why and returns 2.
 */
int run(int argc, char **argv, Body body);

/*! True inside run(): the server is the service and has no console. */
bool active();

/*! Tells the manager the service is up: called once the port is taken. */
void reportRunning();

/*! The manager asked the service to stop (or the machine shuts down). */
bool stopRequested();

} // namespace Service
