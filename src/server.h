#pragma once

#include "config.h"
#include "httpserver.h"

#include <QObject>
#include <QString>

#include <memory>
#include <vector>

class Api;
class MetricsRecorder;
class QTimer;
class Storage;
class WorkerPool;
struct LoginThrottle;

/*!
 * The server as a whole: the base, the threads that answer requests, the
 * HTTP side on the thread of the caller, and the metrics on a thread of
 * their own. main() runs one; the tests start one on a free port.
 *
 * Every worker thread has its own connection to the base and its own Api on
 * it; they share the gate of the writes (Storage::Write) and the memory of
 * wrong passwords. The connection of the caller's thread (storage()) is the
 * one that brought the schema up to date.
 */
class Server : public QObject
{
    Q_OBJECT

public:
    /*! What the tests change and the settings do not. */
    struct Tuning
    {
        int metricsTickMs = 5000;    //!< 0: the tests tick by hand
        bool metrics = true;
        int flushMs = 5000;          //!< how often the counted exchanges are written
        bool customLimits = false;   //!< take limits instead of those of the config
        HttpServer::Limits limits;
    };

    explicit Server(const ServerConfig &config, QObject *parent = nullptr);
    ~Server() override;

    void setTuning(const Tuning &tuning) { m_tuning = tuning; }

    /*! Opens the base, starts the threads and listens on the host and port of the config. */
    bool start(QString *error = nullptr);
    void stop();

    quint16 port() const;
    const ServerConfig &config() const { return m_config; }

    Storage &storage() { return *m_storage; }
    HttpServer &http() { return *m_http; }
    WorkerPool &pool() { return *m_pool; }
    MetricsRecorder *metrics() { return m_metrics.get(); }

    /*! Writes the exchanges counted in memory now, rather than at the next tick. */
    void flush();

private:
    void scheduleFlush();

    ServerConfig m_config;
    Tuning m_tuning;
    std::unique_ptr<Storage> m_storage;
    std::shared_ptr<LoginThrottle> m_throttle;

    /*! One per thread of the pool, touched only by that thread. */
    struct Worker
    {
        std::unique_ptr<Storage> storage;
        std::unique_ptr<Api> api;
    };
    std::vector<Worker> m_workers;

    std::unique_ptr<WorkerPool> m_pool;
    std::unique_ptr<HttpServer> m_http;
    std::unique_ptr<MetricsRecorder> m_metrics;
    QTimer *m_flushTimer = nullptr;
    bool m_started = false;
};
