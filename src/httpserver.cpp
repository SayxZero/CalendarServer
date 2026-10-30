#include "httpserver.h"

#include "workerpool.h"

#include <QDateTime>
#include <QLocale>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <new>

namespace {

const int kMaxHeaderBytes = 32 * 1024;

/*!
 * What Qt reads ahead of the server from one socket. Past it the socket is
 * not read, the receive window of TCP closes and the client waits: the way
 * a body that does not fit the budget yet is held back.
 */
const qint64 kSocketReadAhead = 64 * 1024;

/*! Answers waiting to leave one connection: past the first no new request of it is taken. */
const qint64 kWriteHighWater = 4 * 1024 * 1024;
const qint64 kWriteLowWater = 1024 * 1024;

/*!
 * At the connection limit a kept-alive connection silent for this long is
 * closed to let a waiting one in: its client opens a new one when it needs
 * it, which costs it a handshake, not an answer.
 */
const qint64 kEvictIdleAfterMs = 1000;

QByteArray reasonPhrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default:  return "Status";
    }
}

/*! RFC 7231 date, always in English whatever the locale of the machine. */
QByteArray httpDate()
{
    return QLocale::c()
        .toString(QDateTime::currentDateTimeUtc(), QStringLiteral("ddd, dd MMM yyyy hh:mm:ss"))
        .toLatin1()
        + " GMT";
}

template <typename T>
void raiseTo(std::atomic<T> &maximum, T value)
{
    T seen = maximum.load();
    while (value > seen && !maximum.compare_exchange_weak(seen, value)) {
    }
}

/*!
 * Whose turn it is in the fair line of the pool: the session, however many
 * connections it comes through. A request without one (a login) takes its
 * turn as its connection: by the address, every login behind one proxy or
 * router would make one client and wait for all of them in turn.
 */
QString clientKey(const HttpRequest &request, quint64 connection)
{
    const QByteArray authorization = request.header("authorization");
    if (!authorization.isEmpty())
        return QString::fromLatin1(authorization);
    return QStringLiteral("connection %1").arg(connection);
}

/*! A dual stack socket reports IPv4 peers as "::ffff:a.b.c.d": back to a.b.c.d. */
QHostAddress plainAddress(QHostAddress address)
{
    bool mapped = false;
    const quint32 ipv4 = address.toIPv4Address(&mapped);
    if (mapped)
        address = QHostAddress(ipv4);
    return address;
}

/*!
 * One entry of X-Forwarded-For. Some proxies add the port, "1.2.3.4:5678"
 * or "[::1]:5678"; a name, "unknown" or anything else is a null address.
 */
QHostAddress forwardedAddress(const QByteArray &entry)
{
    QByteArray text = entry.trimmed();
    if (text.startsWith('[')) {
        const int close = text.indexOf(']');
        text = close > 0 ? text.mid(1, close - 1) : QByteArray();
    } else if (text.count(':') == 1) {
        text.truncate(text.indexOf(':'));
    }
    QHostAddress address;
    if (text.isEmpty() || !address.setAddress(QString::fromLatin1(text)))
        return QHostAddress();
    return plainAddress(address);
}

} // namespace

HttpResponse HttpResponse::json(int status, const QByteArray &body)
{
    HttpResponse response;
    response.status = status;
    response.body = body;
    return response;
}

HttpResponse HttpResponse::overloaded(int retryAfterSeconds)
{
    HttpResponse response = json(
        503, QStringLiteral("{\"error\":\"overloaded\",\"message\":\"Сервер перегружен, "
                            "повторите попытку чуть позже\"}")
                 .toUtf8());
    response.headers.insert("Retry-After", QByteArray::number(retryAfterSeconds));
    return response;
}

HttpServer::HttpServer(QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_evictTimer(new QTimer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &HttpServer::acceptConnections);
    m_evictTimer->setInterval(250);
    connect(m_evictTimer, &QTimer::timeout, this, &HttpServer::evictIdle);
}

HttpServer::~HttpServer()
{
    close();
}

void HttpServer::setPool(WorkerPool *pool, WorkerHandler handler)
{
    m_pool = pool;
    m_workerHandler = std::move(handler);
}

void HttpServer::setLimits(const Limits &limits)
{
    m_limits = limits;
    m_limits.maxConnections = qMax(1, m_limits.maxConnections);
    m_limits.bodyMemoryFactor = qMax(1, m_limits.bodyMemoryFactor);
    // No need to grow the budget to the biggest body: reserve() lets one body
    // in alone, and a budget grown past the memory there is would let several.
    m_limits.bufferBudget = qMax<qint64>(1, m_limits.bufferBudget);
}

void HttpServer::setMaxBodySize(qint64 bytes)
{
    Limits limits = m_limits;
    limits.maxBody = bytes;
    setLimits(limits);
}

void HttpServer::setTrustedProxies(const QList<QPair<QHostAddress, int>> &proxies)
{
    m_trustedProxies = proxies;
}

bool HttpServer::isTrustedProxy(const QHostAddress &address) const
{
    return std::any_of(m_trustedProxies.cbegin(), m_trustedProxies.cend(),
                       [&](const QPair<QHostAddress, int> &subnet) {
                           return address.isInSubnet(subnet.first, subnet.second);
                       });
}

QHostAddress HttpServer::clientOf(const HttpRequest &request) const
{
    // From the right: each entry was added by the hop after it, and is worth
    // what that hop is - a client can write anything at the left end.
    QHostAddress client = request.peer;
    if (m_trustedProxies.isEmpty() || !isTrustedProxy(client))
        return client;
    const QList<QByteArray> hops = request.header("x-forwarded-for").split(',');
    for (auto hop = hops.crbegin(); hop != hops.crend() && isTrustedProxy(client); ++hop) {
        const QHostAddress address = forwardedAddress(*hop);
        if (address.isNull())
            break;
        client = address;
    }
    return client;
}

bool HttpServer::listen(const QHostAddress &address, quint16 port)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    // While accepting pauses, new connections wait here; the system caps it.
    m_server->setListenBacklogSize(1024);
#endif
    return m_server->listen(address, port);
}

void HttpServer::close()
{
    m_server->close();
    m_evictTimer->stop();
    const QList<quint64> ids = m_connections.keys();
    for (quint64 id : ids)
        dropConnection(id);
}

quint16 HttpServer::serverPort() const
{
    return m_server->serverPort();
}

QString HttpServer::errorString() const
{
    return m_server->errorString();
}

HttpSnapshot HttpServer::snapshot(bool takeMaxima)
{
    HttpSnapshot shot;
    shot.connections = m_connectionCount.load();
    shot.maxConnections = takeMaxima ? m_maxConnectionsSeen.exchange(shot.connections)
                                     : m_maxConnectionsSeen.load();
    shot.connectionLimit = m_limits.maxConnections;
    shot.paused = m_pausedFlag.load();
    shot.accepted = m_accepted.load();
    shot.requests = m_requests.load();
    shot.clientErrors = m_clientErrors.load();
    shot.serverErrors = m_serverErrors.load();
    shot.overloaded = m_overloaded.load();
    shot.bytesIn = m_bytesIn.load();
    shot.bytesOut = m_bytesOut.load();
    shot.buffered = m_bufferedFlag.load();
    shot.bufferBudget = m_limits.bufferBudget;
    return shot;
}

// ---------------------------------------------------------------- connections

void HttpServer::acceptConnections()
{
    while (m_connections.size() < m_limits.maxConnections) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        if (!socket)
            break;
        socket->setReadBufferSize(kSocketReadAhead);

        Connection connection;
        connection.id = ++m_nextId;
        connection.socket = socket;
        connection.lastActiveMs = QDateTime::currentMSecsSinceEpoch();
        connection.gone = std::make_shared<std::atomic<bool>>(false);
        connection.idle = new QTimer(socket);
        connection.idle->setSingleShot(true);
        connection.idle->setInterval(m_limits.idleTimeoutMs);
        const quint64 id = connection.id;
        connect(connection.idle, &QTimer::timeout, this, [this, id] { dropConnection(id); });
        connection.idle->start();
        m_connections.insert(id, connection);

        connect(socket, &QTcpSocket::readyRead, this, [this, id] { process(id); });
        connect(socket, &QTcpSocket::disconnected, this, [this, id] { dropConnection(id); });
        connect(socket, &QTcpSocket::bytesWritten, this, [this, id] {
            const auto it = m_connections.find(id);
            if (it == m_connections.end() || !it->waitingWrite)
                return;
            if (it->socket->bytesToWrite() <= kWriteLowWater) {
                it->waitingWrite = false;
                process(id);
            }
        });

        m_accepted.fetch_add(1);
        m_connectionCount.store(int(m_connections.size()));
        raiseTo(m_maxConnectionsSeen, int(m_connections.size()));

        // A connection Qt took in while the server was at its limit has its
        // request read already: readyRead was emitted before anybody
        // listened, and no more data is coming to emit it again.
        if (socket->bytesAvailable() > 0)
            QMetaObject::invokeMethod(this, [this, id] { process(id); }, Qt::QueuedConnection);
    }

    // At the limit: the rest wait in the queue of the system, which is a slow
    // start for them rather than a refusal.
    if (m_connections.size() >= m_limits.maxConnections && !m_paused) {
        pauseAccepting();
        // Under a long load the limit is reached again and again: a line a
        // minute is enough to tell.
        ++m_pausesUnreported;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_pauseReportedMs >= 60 * 1000) {
            qWarning().noquote() << QStringLiteral("Соединений %1 — предел; новые ждут в очереди "
                                                   "системы (раз за минуту: %2)")
                                        .arg(m_connections.size())
                                        .arg(m_pausesUnreported);
            m_pauseReportedMs = now;
            m_pausesUnreported = 0;
        }
    }
}

void HttpServer::pauseAccepting()
{
    m_server->pauseAccepting();
    m_paused = true;
    m_pausedFlag = true;
    m_evictTimer->start();
    evictIdle();
}

void HttpServer::resumeAccepting()
{
    if (!m_paused || m_connections.size() >= m_limits.maxConnections || !m_server->isListening())
        return;
    m_paused = false;
    m_pausedFlag = false;
    m_evictTimer->stop();
    m_server->resumeAccepting();
    // Those Qt took in already are waiting in its own list.
    QMetaObject::invokeMethod(this, &HttpServer::acceptConnections, Qt::QueuedConnection);
}

void HttpServer::evictIdle()
{
    if (!m_paused)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<quint64> idle;
    for (const Connection &connection : std::as_const(m_connections)) {
        if (!connection.busy && !connection.haveHead && connection.buffer.isEmpty()
            && connection.socket->bytesAvailable() == 0 && connection.socket->bytesToWrite() == 0
            && now - connection.lastActiveMs >= kEvictIdleAfterMs) {
            idle.append(connection.id);
        }
    }
    for (quint64 id : idle)
        dropConnection(id);
}

void HttpServer::dropConnection(quint64 id)
{
    const auto it = m_connections.find(id);
    if (it == m_connections.end())
        return;
    Connection connection = *it;
    m_connections.erase(it);
    m_connectionCount.store(int(m_connections.size()));

    // A request of it still waiting in the pool is not worth running now.
    connection.gone->store(true);
    if (connection.admitted && !connection.busy)
        release(connection.reserved);
    if (connection.waitingBudget) {
        const auto queued = std::find(m_budgetLine.begin(), m_budgetLine.end(), id);
        if (queued != m_budgetLine.end())
            m_budgetLine.erase(queued);
    }

    QTcpSocket *socket = connection.socket;
    socket->disconnect(this);
    if (socket->state() != QAbstractSocket::UnconnectedState)
        socket->disconnectFromHost();
    socket->deleteLater();
    resumeAccepting();
}

// ------------------------------------------------------------------- requests

void HttpServer::process(quint64 id)
{
    try {
        processRequests(id);
    } catch (const std::bad_alloc &) {
        dropForMemory(id);
    }
}

void HttpServer::dropForMemory(quint64 id)
{
    m_overloaded.fetch_add(1);
    try {
        dropConnection(id);
        qWarning().noquote() << QStringLiteral("Не хватило памяти на запрос: соединение закрыто");
    } catch (...) {
        // Not even that much memory: the connection stays, and its timer closes it.
    }
}

void HttpServer::processRequests(quint64 id)
{
    for (;;) {
        const auto it = m_connections.find(id);
        if (it == m_connections.end())
            return;
        Connection &connection = *it;
        if (connection.busy || connection.waitingBudget || connection.waitingWrite)
            return;

        pull(connection);
        HttpRequest request;
        const int state = parseRequest(connection, request);
        if (state == 0)
            return;
        if (state != 1) {
            const QByteArray message = QByteArrayLiteral("{\"error\":\"http\",\"message\":\"")
                                       + reasonPhrase(state) + "\"}";
            m_requests.fetch_add(1);
            m_clientErrors.fetch_add(1);
            reply(connection.socket, HttpResponse::json(state, message), false);
            closeAfterWrite(connection);
            return;
        }
        if (!dispatch(connection, std::move(request)))
            return;
        // Answered at once: the next request of the connection may be there.
    }
}

void HttpServer::pull(Connection &connection)
{
    QTcpSocket *socket = connection.socket;
    const qint64 available = socket->bytesAvailable();
    if (available <= 0)
        return;

    // The head is read up to its limit; a body only once it fits the budget,
    // and no further than its end: what lies beyond is the next request,
    // read when this one is done.
    qint64 wanted = 0;
    if (!connection.haveHead)
        wanted = kMaxHeaderBytes + 4 - connection.buffer.size();
    else if (connection.admitted)
        wanted = connection.headEnd + 4 + connection.contentLength - connection.buffer.size();
    if (wanted <= 0)
        return;
    // A body grown chunk by chunk would be copied over as it doubles.
    if (connection.haveHead && connection.admitted)
        connection.buffer.reserve(connection.headEnd + 4 + connection.contentLength);

    const QByteArray chunk = socket->read(qMin(available, wanted));
    if (chunk.isEmpty())
        return;
    connection.lastActiveMs = QDateTime::currentMSecsSinceEpoch();
    if (connection.buffer.isEmpty() && !connection.haveHead)
        connection.requestStartedMs = connection.lastActiveMs;
    connection.buffer.append(chunk);
    m_bytesIn.fetch_add(chunk.size());
    connection.idle->start();
}

int HttpServer::parseRequest(Connection &connection, HttpRequest &request)
{
    QByteArray &buffer = connection.buffer;

    if (!connection.haveHead) {
        const int headEnd = buffer.indexOf("\r\n\r\n");
        if (headEnd < 0) {
            if (buffer.size() > kMaxHeaderBytes)
                return 431;
            // Headers trickling in a byte at a time hold a connection for
            // nothing: they have a deadline of their own.
            if (!buffer.isEmpty() && connection.requestStartedMs >= 0
                && QDateTime::currentMSecsSinceEpoch() - connection.requestStartedMs
                       > m_limits.headerTimeoutMs) {
                return 408;
            }
            return 0;
        }
        if (headEnd > kMaxHeaderBytes)
            return 431;

        const QList<QByteArray> lines = buffer.left(headEnd).split('\n');
        if (lines.isEmpty())
            return 400;
        const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
        if (requestLine.size() != 3 || !requestLine.at(2).startsWith("HTTP/1."))
            return 400;

        QHash<QByteArray, QByteArray> headers;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            if (line.isEmpty())
                continue;
            const int colon = line.indexOf(':');
            if (colon <= 0)
                return 400;
            headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
        }

        const QByteArray transfer = headers.value("transfer-encoding").toLower();
        if (!transfer.isEmpty() && transfer != "identity")
            return 501;

        qint64 length = 0;
        if (headers.contains("content-length")) {
            bool ok = false;
            length = headers.value("content-length").toLongLong(&ok);
            if (!ok || length < 0)
                return 400;
        }
        if (length > m_limits.maxBody)
            return 413;

        HttpRequest &head = connection.head;
        head = HttpRequest();
        head.method = requestLine.at(0).toUpper();
        head.version = requestLine.at(2).toUpper();
        const QByteArray target = requestLine.at(1);
        const int question = target.indexOf('?');
        const QByteArray rawPath = question < 0 ? target : target.left(question);
        head.path = QUrl::fromPercentEncoding(rawPath);
        if (question >= 0)
            head.query = QUrlQuery(QString::fromUtf8(target.mid(question + 1)));
        head.headers = headers;

        connection.haveHead = true;
        connection.headEnd = headEnd;
        connection.contentLength = length;
        connection.reserved = 0;
        const qint64 cost = length * m_limits.bodyMemoryFactor;
        connection.admitted = length == 0 || reserve(cost);
        if (connection.admitted) {
            connection.reserved = length == 0 ? 0 : cost;
        } else {
            // The body waits until the bodies before it are done with.
            connection.waitingBudget = true;
            m_budgetLine.push_back(connection.id);
        }
        if (!connection.admitted)
            return 0;
        pull(connection);
    }

    const qint64 total = connection.headEnd + 4 + connection.contentLength;
    if (buffer.size() < total)
        return 0;

    request = std::move(connection.head);
    connection.head = HttpRequest();
    if (buffer.size() == total) {
        // The usual case, nothing read past the body: the buffer becomes the
        // body, without a copy of it in memory next to the buffer.
        buffer.remove(0, connection.headEnd + 4);
        request.body = std::move(buffer);
        buffer = QByteArray();
    } else {
        request.body = buffer.mid(connection.headEnd + 4, connection.contentLength);
        buffer.remove(0, int(total));
    }

    connection.haveHead = false;
    connection.headEnd = 0;
    connection.contentLength = 0;
    connection.requestStartedMs = buffer.isEmpty() ? -1 : QDateTime::currentMSecsSinceEpoch();
    return 1;
}

bool HttpServer::dispatch(Connection &connection, HttpRequest request)
{
    request.peer = plainAddress(connection.socket->peerAddress());
    request.client = clientOf(request);
    // HTTP/1.1 keeps the connection unless told otherwise, HTTP/1.0 closes
    // it unless asked to keep it.
    const QByteArray connectionHeader = request.header("connection").toLower();
    const bool keepAlive = request.version == "HTTP/1.0" ? connectionHeader == "keep-alive"
                                                         : connectionHeader != "close";

    const quint64 id = connection.id;
    const qint64 reserved = connection.reserved;
    connection.reserved = 0;
    connection.admitted = false;
    connection.busy = true;
    connection.idle->stop();

    if (!m_pool) {
        const HttpResponse response =
            m_handler ? m_handler(request) : HttpResponse::json(503, "{\"error\":\"unavailable\"}");
        deliver(id, response, keepAlive, reserved);
        return true;
    }

    PoolTask task;
    task.client = clientKey(request, id);
    task.cancelled = connection.gone;
    const qint64 maxWaitNs = qint64(m_limits.maxQueueWaitMs) * 1000000;
    const int retryAfter = m_limits.retryAfterSeconds;
    task.run = [this, id, keepAlive, reserved, maxWaitNs, retryAfter,
                request = std::move(request)](int worker, qint64 waitedNs) {
        HttpResponse response;
        bool late = false;
        if (waitedNs > maxWaitNs) {
            // The client has given up long ago: the work would be for nothing,
            // and the line behind it would only grow.
            late = true;
            response = HttpResponse::overloaded(retryAfter);
        } else {
            try {
                response = m_workerHandler(worker, request);
            } catch (const std::bad_alloc &) {
                response = HttpResponse::overloaded(retryAfter);
                late = true;
            }
        }
        if (late)
            m_overloaded.fetch_add(1);
        QMetaObject::invokeMethod(
            this,
            [this, id, keepAlive, reserved, response = std::move(response)] {
                try {
                    deliver(id, response, keepAlive, reserved);
                } catch (const std::bad_alloc &) {
                    dropForMemory(id);
                    return;
                }
                process(id);
            },
            Qt::QueuedConnection);
    };
    task.cancel = [this, reserved] {
        QMetaObject::invokeMethod(this, [this, reserved] { release(reserved); },
                                  Qt::QueuedConnection);
    };

    bool queued = false;
    try {
        queued = m_pool->submit(std::move(task));
    } catch (const std::bad_alloc &) {
        // Not queued: the reservation would be held by nobody for ever.
        if (const auto it = m_connections.find(id); it != m_connections.end())
            it->busy = false;
        release(reserved);
        throw;
    }
    if (queued)
        return false;

    // The line is full: answered at once, and the client tries again later.
    m_overloaded.fetch_add(1);
    deliver(id, HttpResponse::overloaded(m_limits.retryAfterSeconds), keepAlive, reserved);
    return true;
}

void HttpServer::deliver(quint64 id, const HttpResponse &response, bool keepAlive, qint64 reserved)
{
    release(reserved);
    m_requests.fetch_add(1);
    if (response.status >= 500)
        m_serverErrors.fetch_add(1);
    else if (response.status >= 400)
        m_clientErrors.fetch_add(1);

    const auto it = m_connections.find(id);
    if (it == m_connections.end())
        return;   // the client went away meanwhile
    Connection &connection = *it;
    connection.busy = false;
    connection.lastActiveMs = QDateTime::currentMSecsSinceEpoch();
    reply(connection.socket, response, keepAlive);
    if (!keepAlive) {
        closeAfterWrite(connection);
        return;
    }
    connection.idle->start();
    if (connection.socket->bytesToWrite() > kWriteHighWater)
        connection.waitingWrite = true;
}

void HttpServer::reply(QTcpSocket *socket, const HttpResponse &response, bool keepAlive)
{
    QByteArray head;
    head += "HTTP/1.1 " + QByteArray::number(response.status) + ' '
            + reasonPhrase(response.status) + "\r\n";
    head += "Server: CalendarServer/" SERVER_VERSION "\r\n";
    head += "Date: " + httpDate() + "\r\n";
    if (!response.body.isEmpty() || response.status != 204)
        head += "Content-Type: " + response.contentType + "\r\n";
    head += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\n";
    for (auto it = response.headers.constBegin(); it != response.headers.constEnd(); ++it)
        head += it.key() + ": " + it.value() + "\r\n";
    head += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    head += "\r\n";

    socket->write(head);
    qint64 sent = head.size();
    if (!response.headOnly) {
        socket->write(response.body);
        sent += response.body.size();
    }
    m_bytesOut.fetch_add(sent);
}

void HttpServer::closeAfterWrite(Connection &connection)
{
    // Let the answer leave before the connection goes.
    const quint64 id = connection.id;
    QTcpSocket *socket = connection.socket;
    connection.idle->stop();
    connection.gone->store(true);
    if (connection.admitted)
        release(connection.reserved);
    if (connection.waitingBudget) {
        const auto queued = std::find(m_budgetLine.begin(), m_budgetLine.end(), id);
        if (queued != m_budgetLine.end())
            m_budgetLine.erase(queued);
    }
    m_connections.remove(id);
    m_connectionCount.store(int(m_connections.size()));
    socket->disconnect(this);
    connect(socket, &QTcpSocket::bytesWritten, socket, [socket] {
        if (socket->bytesToWrite() == 0)
            socket->disconnectFromHost();
    });
    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    if (socket->bytesToWrite() == 0)
        socket->disconnectFromHost();
    resumeAccepting();
}

// --------------------------------------------------------------------- budget

bool HttpServer::reserve(qint64 bytes)
{
    // One body is always let in, however big: with nothing held there is
    // nobody to wait for.
    if (m_reserved > 0 && m_reserved + bytes > m_limits.bufferBudget)
        return false;
    m_reserved += bytes;
    m_bufferedFlag.store(m_reserved);
    return true;
}

void HttpServer::release(qint64 bytes)
{
    if (bytes <= 0)
        return;
    m_reserved = qMax<qint64>(0, m_reserved - bytes);
    m_bufferedFlag.store(m_reserved);
    admitWaiting();
}

void HttpServer::admitWaiting()
{
    while (!m_budgetLine.empty()) {
        const quint64 id = m_budgetLine.front();
        const auto it = m_connections.find(id);
        if (it == m_connections.end()) {
            m_budgetLine.pop_front();
            continue;
        }
        const qint64 cost = it->contentLength * m_limits.bodyMemoryFactor;
        if (!reserve(cost))
            return;
        m_budgetLine.pop_front();
        it->reserved = cost;
        it->admitted = true;
        it->waitingBudget = false;
        // Not from here: this runs inside the answer of another connection.
        QMetaObject::invokeMethod(this, [this, id] { process(id); }, Qt::QueuedConnection);
    }
}
