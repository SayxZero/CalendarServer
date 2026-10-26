#include "httpserver.h"

#include <QDateTime>
#include <QLocale>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

namespace {

const int kMaxHeaderBytes = 32 * 1024;
const int kIdleTimeoutMs = 60 * 1000;
const int kMaxConnections = 512;

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

} // namespace

HttpResponse HttpResponse::json(int status, const QByteArray &body)
{
    HttpResponse response;
    response.status = status;
    response.body = body;
    return response;
}

HttpServer::HttpServer(QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &HttpServer::acceptConnections);
}

HttpServer::~HttpServer()
{
    close();
}

bool HttpServer::listen(const QHostAddress &address, quint16 port)
{
    return m_server->listen(address, port);
}

void HttpServer::close()
{
    m_server->close();
    const QList<QTcpSocket *> sockets = m_connections.keys();
    for (QTcpSocket *socket : sockets)
        dropConnection(socket);
}

quint16 HttpServer::serverPort() const
{
    return m_server->serverPort();
}

QString HttpServer::errorString() const
{
    return m_server->errorString();
}

void HttpServer::acceptConnections()
{
    while (QTcpSocket *socket = m_server->nextPendingConnection()) {
        if (m_connections.size() >= kMaxConnections) {
            socket->abort();
            socket->deleteLater();
            continue;
        }

        Connection connection;
        connection.socket = socket;
        connection.idle = new QTimer(socket);
        connection.idle->setSingleShot(true);
        connection.idle->setInterval(kIdleTimeoutMs);
        connect(connection.idle, &QTimer::timeout, this, [this, socket] { dropConnection(socket); });
        connection.idle->start();
        m_connections.insert(socket, connection);

        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { readFrom(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { dropConnection(socket); });
    }
}

void HttpServer::dropConnection(QTcpSocket *socket)
{
    const auto it = m_connections.find(socket);
    if (it == m_connections.end())
        return;
    m_connections.erase(it);
    socket->disconnect(this);
    if (socket->state() != QAbstractSocket::UnconnectedState)
        socket->disconnectFromHost();
    socket->deleteLater();
}

void HttpServer::readFrom(QTcpSocket *socket)
{
    const auto it = m_connections.find(socket);
    if (it == m_connections.end())
        return;

    it->buffer.append(socket->readAll());
    it->idle->start();

    // A client may send several requests back to back; answer them in order.
    while (m_connections.contains(socket)) {
        Connection &connection = m_connections[socket];
        HttpRequest request;
        const int state = parseRequest(connection.buffer, request);
        if (state == 0)
            return;

        if (state != 1) {
            const QByteArray message = QByteArrayLiteral("{\"error\":\"http\",\"message\":\"")
                                       + reasonPhrase(state) + "\"}";
            send(socket, HttpResponse::json(state, message), false);
            return;
        }

        // A dual stack socket reports IPv4 peers as "::ffff:a.b.c.d".
        request.peer = socket->peerAddress();
        bool mapped = false;
        const quint32 ipv4 = request.peer.toIPv4Address(&mapped);
        if (mapped)
            request.peer = QHostAddress(ipv4);
        // HTTP/1.1 keeps the connection unless told otherwise, HTTP/1.0 closes
        // it unless asked to keep it.
        const QByteArray connectionHeader = request.header("connection").toLower();
        const bool keepAlive = request.version == "HTTP/1.0" ? connectionHeader == "keep-alive"
                                                             : connectionHeader != "close";

        HttpResponse response;
        if (m_handler) {
            response = m_handler(request);
        } else {
            response = HttpResponse::json(503, "{\"error\":\"unavailable\"}");
        }
        send(socket, response, keepAlive);
        if (!keepAlive)
            return;
    }
}

int HttpServer::parseRequest(QByteArray &buffer, HttpRequest &request) const
{
    const int headEnd = buffer.indexOf("\r\n\r\n");
    if (headEnd < 0)
        return buffer.size() > kMaxHeaderBytes ? 431 : 0;
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
    if (length > m_maxBody)
        return 413;

    const qint64 total = headEnd + 4 + length;
    if (buffer.size() < total)
        return 0;

    request.method = requestLine.at(0).toUpper();
    request.version = requestLine.at(2).toUpper();
    const QByteArray target = requestLine.at(1);
    const int question = target.indexOf('?');
    const QByteArray rawPath = question < 0 ? target : target.left(question);
    request.path = QUrl::fromPercentEncoding(rawPath);
    if (question >= 0)
        request.query = QUrlQuery(QString::fromUtf8(target.mid(question + 1)));
    request.headers = headers;
    request.body = buffer.mid(headEnd + 4, length);

    buffer.remove(0, total);
    return 1;
}

void HttpServer::send(QTcpSocket *socket, const HttpResponse &response, bool keepAlive)
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
    if (!response.headOnly)
        socket->write(response.body);

    if (!keepAlive) {
        // Let the answer leave before the connection goes.
        m_connections.remove(socket);
        socket->disconnect(this);
        connect(socket, &QTcpSocket::bytesWritten, socket, [socket] {
            if (socket->bytesToWrite() == 0)
                socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        if (socket->bytesToWrite() == 0)
            socket->disconnectFromHost();
    }
}
