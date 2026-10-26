#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrlQuery>

#include <functional>

class QTcpServer;
class QTcpSocket;
class QTimer;

/*! One parsed HTTP request. Header names are stored in lower case. */
struct HttpRequest
{
    QByteArray method;
    QByteArray version;    //!< "HTTP/1.1" or "HTTP/1.0"
    QString path;          //!< decoded path without the query, e.g. "/api/sync"
    QUrlQuery query;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
    QHostAddress peer;

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
};

/*!
 * A small HTTP/1.1 server on top of QTcpServer: persistent connections,
 * Content-Length bodies, one request handled at a time per connection.
 *
 * That is all the planner needs, and it keeps the server on QtNetwork alone:
 * the QtHttpServer module is missing from the Qt packages of most Linux
 * distributions and changed its API between 6.4 and 6.8.
 */
class HttpServer : public QObject
{
    Q_OBJECT

public:
    using Handler = std::function<HttpResponse(const HttpRequest &)>;

    explicit HttpServer(QObject *parent = nullptr);
    ~HttpServer() override;

    void setHandler(Handler handler) { m_handler = std::move(handler); }

    /*! Largest request body accepted; a bigger one gets 413. */
    void setMaxBodySize(qint64 bytes) { m_maxBody = bytes; }

    bool listen(const QHostAddress &address, quint16 port);
    void close();
    quint16 serverPort() const;
    QString errorString() const;

private:
    struct Connection
    {
        QPointer<QTcpSocket> socket;
        QByteArray buffer;
        QTimer *idle = nullptr;
    };

    void acceptConnections();
    void readFrom(QTcpSocket *socket);
    void dropConnection(QTcpSocket *socket);

    /*!
     * Takes one complete request off the front of @p buffer. Returns 0 while
     * the request is still incomplete, 1 when @p request was filled in, and an
     * HTTP status (400, 413, 431, 501) when the request cannot be served.
     */
    int parseRequest(QByteArray &buffer, HttpRequest &request) const;
    void send(QTcpSocket *socket, const HttpResponse &response, bool keepAlive);

    QTcpServer *m_server = nullptr;
    QHash<QTcpSocket *, Connection> m_connections;
    Handler m_handler;
    qint64 m_maxBody = 64ll * 1024 * 1024;
};
