#pragma once

#include "config.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

/*!
 * The server's service as the admin panel sees and drives it: the service
 * control manager on Windows, systemd elsewhere. The panel only asks for the
 * state and for a start or a stop; the service itself was set up by the
 * installer (or by hand, from deploy/calendar-server.service).
 */
class ServiceControl
{
public:
    enum class State {
        Unknown,   //!< could not be asked, see Status::error
        Missing,   //!< no such service
        Stopped,
        Starting,
        Stopping,
        Running,
    };

    struct Status
    {
        State state = State::Unknown;
        qint64 pid = 0;
        QDateTime startedAt;     //!< when the process of the service started
        bool autoStart = false;  //!< starts with the system
        int exitCode = 0;        //!< what the last run ended with, when it stopped by itself
        QString error;
    };

    explicit ServiceControl(const QString &name = defaultName());

    /*! "CosmosCalendarServer" on Windows, "calendar-server" (the systemd unit) elsewhere. */
    static QString defaultName();
    QString name() const { return m_name; }

    Status status() const;

    /*! The command line the service starts the server with, without the program. */
    QStringList arguments(QString *error = nullptr) const;

    /*! The settings that command line gives: the base, the port, the log. */
    static bool configOf(const QStringList &arguments, ServerConfig *config, QString *error);

    bool start(QString *error);
    bool stop(QString *error);

    /*!
     * Waits while the service is starting or stopping, at most @p timeoutMs,
     * keeping the window of the panel responding, and returns where it ended:
     * a start that failed ends in Stopped, not in a timeout.
     */
    State settle(int timeoutMs) const;

    static QString stateText(State state);

    /*! The panel runs with the rights of an administrator (root elsewhere). */
    static bool isElevated();

    /*!
     * Starts the panel again with the rights of an administrator (the UAC
     * question on Windows). False when it did not happen; @p declined tells
     * whether the user said no.
     */
    static bool relaunchElevated(const QStringList &arguments, bool *declined);

private:
    QString m_name;
};
