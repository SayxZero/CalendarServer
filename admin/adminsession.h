#pragma once

#include "admindb.h"
#include "config.h"
#include "runtimefile.h"
#include "servicecontrol.h"

#include <QDateTime>
#include <QObject>
#include <QString>

class QThread;
class QTimer;

/*! What the server answered on /api/ping the last time it was asked. */
struct PingAnswer
{
    bool answered = false;
    int milliseconds = 0;
    QString version;
    int api = 0;
    bool registration = true;
    QString error;
    QDateTime at;
};

/*!
 * Everything the pages of the panel share: which server it administers (the
 * settings of the service, or a folder given on the command line), its
 * service, the connection to its base and the answers to the regular checks.
 *
 * The state of the service is asked every two seconds and the server pinged
 * once a minute and whenever that state changes; the pages listen to the
 * signals instead of asking themselves.
 */
class AdminSession : public QObject
{
    Q_OBJECT

public:
    /*!
     * @p arguments - the command line the server runs with, as far as its
     * settings go (--config, --data-dir, --port...). @p viaService - they are
     * those of the installed service; otherwise the panel works on a folder of
     * its own choosing and the service is shown for information only.
     */
    AdminSession(const QStringList &arguments, const QString &serviceName, bool viaService,
                 QObject *parent = nullptr);
    ~AdminSession() override;

    const ServerConfig &config() const { return m_config; }

    /*! Reads the settings again: the INI file may have been edited meanwhile. */
    void reloadConfig();

    bool viaService() const { return m_viaService; }
    ServiceControl &service() { return m_service; }
    const ServiceControl::Status &status() const { return m_status; }
    const PingAnswer &ping() const { return m_ping; }

    /*!
     * What the server of this base left in its folder (server.lock,
     * server.run): whether one runs on it right now, whatever started it, and
     * on which port. A console server given -p is found here and nowhere else.
     */
    const RuntimeInfo &runtime() const { return m_runtime; }

    /*!
     * The port to ask: the one the running server of this base listens on,
     * the one of the settings when none runs or it is too old to say.
     */
    quint16 serverPort() const;

    AdminDb &db() { return m_db; }
    QString databasePath() const { return m_config.databasePath(); }

    /*! The log the server writes: the service always does, a console server when told to. */
    QString logPath() const;

    /*! Where backups go: the panel remembers it, by default "backups" in the data folder. */
    QString backupFolder() const;
    void setBackupFolder(const QString &folder);

    /*! Opens the base if it is not open yet; quiet when it does not exist yet. */
    bool ensureDatabase(QString *error = nullptr);

    /*! Lets the base go, for a restore; reopenDatabase() takes it back. */
    void releaseDatabase();
    bool reopenDatabase(QString *error = nullptr);

    /*! Asks for the state at once, not at the next tick. */
    void refreshStatus();
    void refreshPing();

signals:
    void statusChanged();
    void pingChanged();

    /*! The base was opened, reopened or replaced: everything shown from it is stale. */
    void databaseChanged();

    /*! An account changed (a new password): its sessions and so on are stale. */
    void accountsChanged();

private:
    QStringList m_arguments;
    ServerConfig m_config;
    bool m_viaService = false;
    ServiceControl m_service;
    ServiceControl::Status m_status;
    PingAnswer m_ping;
    AdminDb m_db;
    RuntimeInfo m_runtime;
    QThread *m_pingThread = nullptr;
    QTimer *m_statusTimer = nullptr;
    QTimer *m_pingTimer = nullptr;
    bool m_pinging = false;
};
