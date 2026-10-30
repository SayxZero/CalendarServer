#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QUrlQuery>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>

class QTcpServer;
class QTcpSocket;
class QTimer;
class WorkerPool;

/*! One parsed HTTP request. Header names are stored in lower case. */
struct HttpRequest
{
    QByteArray method;
    QByteArray version;    //!< "HTTP/1.1" or "HTTP/1.0"
    QString path;          //!< decoded path without the query, e.g. "/api/sync"
    QUrlQuery query;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
    QHostAddress peer;     //!< the other end of the connection
    /*!
     * Who the request is from: the peer, or behind a trusted proxy the
     * address its X-Forwarded-For names (HttpServer::setTrustedProxies()).
     */
    QHostAddress client;

    QByteArray header(const QByteArray &name) const { return headers.value(name.toLower()); }
};

struct HttpResponse
{
    int status = 200;
    QByteArray contentType = "application/json; charset=utf-8";
    QByteArray body;
    QHash<QByteArray, QByteArray> headers;
    /*!
     * The answer to HEAD: the headers of the GET it stands for, its
     * Content-Length included, and no body.
     */
    bool headOnly = false;

    static HttpResponse json(int status, const QByteArray &body);

    /*! 503 with Retry-After: the server is too busy to take this request now. */
    static HttpResponse overloaded(int retryAfterSeconds);
};

/*! What the HTTP side has seen so far, for the metrics; any thread may ask. */
struct HttpSnapshot
{
    int connections = 0;
    int maxConnections = 0;     //!< the most at once since the previous snapshot that took it
    int connectionLimit = 0;
    bool paused = false;        //!< new connections wait in the system's queue
    qint64 accepted = 0;
    qint64 requests = 0;        //!< answered
    qint64 clientErrors = 0;    //!< 4xx
    qint64 serverErrors = 0;    //!< 5xx, the 503 of an overload too
    qint64 overloaded = 0;      //!< 503 because of the load: a full line or a too long wait
    qint64 bytesIn = 0;
    qint64 bytesOut = 0;
    qint64 buffered = 0;        //!< request bodies held right now
    qint64 bufferBudget = 0;
};

/*!
 * A small HTTP/1.1 server on top of QTcpServer: persistent connections,
 * Content-Length bodies, one request of a connection at a time.
 *
 * That is all the planner needs, and it keeps the server on QtNetwork alone:
 * the QtHttpServer module is missing from the Qt packages of most Linux
 * distributions and changed its API between 6.4 and 6.8.
 *
 * The sockets live on the thread of the server, which only reads, parses and
 * writes. The requests themselves go to a WorkerPool (setPool()), and the
 * answers come back here. A server without a pool answers on its own thread.
 *
 * Under load it slows down rather than falls over: past the connection limit
 * new connections wait in the queue of the system (accepting pauses) while
 * kept-alive connections idle for a second make room,
 * request bodies above the memory budget wait until the requests before them
 * are done (the socket is not read, and TCP slows the client down), a full
 * line of the pool and a request that waited longer than a client waits get
 * 503 with Retry-After at once.
 */
class HttpServer : public QObject
{
    Q_OBJECT

public:
    using Handler = std::function<HttpResponse(const HttpRequest &)>;
    using WorkerHandler = std::function<HttpResponse(int worker, const HttpRequest &)>;

    struct Limits
    {
        qint64 maxBody = 64ll * 1024 * 1024;        //!< a bigger request body gets 413
        int maxConnections = 1024;                   //!< past it accepting pauses
        /*!
         * The memory requests may hold at once: a body counts @c bodyMemoryFactor
         * times its size from the moment its head is read until its answer
         * leaves - its JSON and records while it is answered weigh as much.
         * One body is let in whatever its size when nothing else is held.
         */
        qint64 bufferBudget = 256ll * 1024 * 1024;
        int bodyMemoryFactor = 8;
        int idleTimeoutMs = 60 * 1000;               //!< a silent connection is closed
        int headerTimeoutMs = 20 * 1000;             //!< the headers of a request must arrive by then
        int maxQueueWaitMs = 30 * 1000;              //!< a request that waited longer is not run
        int retryAfterSeconds = 5;                   //!< what 503 tells the client
    };

    explicit HttpServer(QObject *parent = nullptr);
    ~HttpServer() override;

    /*! Requests are answered on this thread. */
    void setHandler(Handler handler) { m_handler = std::move(handler); }

    /*! Requests go to @p pool and are answered by @p handler on its threads. */
    void setPool(WorkerPool *pool, WorkerHandler handler);

    void setLimits(const Limits &limits);
    const Limits &limits() const { return m_limits; }

    /*! Largest request body accepted; a bigger one gets 413. */
    void setMaxBodySize(qint64 bytes);

    /*!
     * Proxies whose X-Forwarded-For is believed; before listen(). The client
     * of a request from one of them is the last address of the header, and
     * while that is a trusted proxy too, the one before it: what the last
     * trusted hop saw. A request from anywhere else, or one whose header
     * cannot be read, is its peer's.
     */
    void setTrustedProxies(const QList<QPair<QHostAddress, int>> &proxies);

    bool listen(const QHostAddress &address, quint16 port);
    void close();
    quint16 serverPort() const;
    QString errorString() const;

    /*! @p takeMaxima resets the maxima, for the next interval of the metrics. */
    HttpSnapshot snapshot(bool takeMaxima = false);

private:
    struct Connection
    {
        quint64 id = 0;
        QTcpSocket *socket = nullptr;
        QByteArray buffer;
        QTimer *idle = nullptr;
        qint64 lastActiveMs = 0;        //!< the last byte read or answer sent
        qint64 requestStartedMs = -1;   //!< the first byte of the request being received

        // The head of a request whose body is still on the way.
        bool haveHead = false;
        HttpRequest head;
        int headEnd = 0;
        qint64 contentLength = 0;
        bool admitted = false;          //!< its body fits the budget and may be read
        qint64 reserved = 0;            //!< of the budget, for that body

        bool busy = false;              //!< a request of it is being answered
        bool waitingBudget = false;
        bool waitingWrite = false;      //!< too much of the answers not sent yet
        std::shared_ptr<std::atomic<bool>> gone;
    };

    void acceptConnections();
    void dropConnection(quint64 id);
    void pauseAccepting();
    void resumeAccepting();

    /*! At the limit: closes the kept-alive connections that idle, to let the waiting in. */
    void evictIdle();

    /*!
     * Reads, parses and dispatches the requests of one connection, as far as
     * it can go. Memory running out on the way closes that connection only.
     */
    void process(quint64 id);
    void processRequests(quint64 id);

    /*!
     * An allocation failed on this thread: nothing above the event loop would
     * catch it, and the process would end. The connection goes instead.
     */
    void dropForMemory(quint64 id);

    /*! What the connection is allowed to read now, and reads it. */
    void pull(Connection &connection);

    /*!
     * Takes one complete request off the front of the buffer. Returns 0 while
     * it is incomplete or waits for the budget, 1 when @p request was filled
     * in, and an HTTP status (400, 408, 413, 431, 501) when it cannot be served.
     */
    int parseRequest(Connection &connection, HttpRequest &request);

    /*! Returns true when the answer was given at once (no pool, or the line is full). */
    bool dispatch(Connection &connection, HttpRequest request);

    void deliver(quint64 id, const HttpResponse &response, bool keepAlive, qint64 reserved);
    void reply(QTcpSocket *socket, const HttpResponse &response, bool keepAlive);
    void closeAfterWrite(Connection &connection);

    bool reserve(qint64 bytes);
    void release(qint64 bytes);
    void admitWaiting();

    bool isTrustedProxy(const QHostAddress &address) const;
    QHostAddress clientOf(const HttpRequest &request) const;

    QTcpServer *m_server = nullptr;
    QHash<quint64, Connection> m_connections;
    quint64 m_nextId = 0;
    std::deque<quint64> m_budgetLine;
    Handler m_handler;
    WorkerHandler m_workerHandler;
    WorkerPool *m_pool = nullptr;
    Limits m_limits;
    QList<QPair<QHostAddress, int>> m_trustedProxies;
    qint64 m_reserved = 0;
    bool m_paused = false;
    QTimer *m_evictTimer = nullptr;
    qint64 m_pauseReportedMs = 0;
    int m_pausesUnreported = 0;

    std::atomic<int> m_connectionCount { 0 };
    std::atomic<int> m_maxConnectionsSeen { 0 };
    std::atomic<bool> m_pausedFlag { false };
    std::atomic<qint64> m_accepted { 0 };
    std::atomic<qint64> m_requests { 0 };
    std::atomic<qint64> m_clientErrors { 0 };
    std::atomic<qint64> m_serverErrors { 0 };
    std::atomic<qint64> m_overloaded { 0 };
    std::atomic<qint64> m_bytesIn { 0 };
    std::atomic<qint64> m_bytesOut { 0 };
    std::atomic<qint64> m_bufferedFlag { 0 };
};
