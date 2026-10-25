#include "api.h"
#include "config.h"
#include "httpserver.h"
#include "storage.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtTest>

/*!
 * The server is started in-process on a free port and talked to over real
 * HTTP, so the parser, the routes and the storage are tested together.
 */
class ServerTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void pingAnswers();
    void registerAndLogin();
    void rejectsBadCredentialsAndInput();
    void requiresToken();
    void syncRoundTrip();
    void oneRecordIsAnsweredAlone();
    void newestChangeWins();
    void deletionsTravel();
    void logoutEndsSession();
    void passwordChangeLogsOutOthers();
    void throttlesGuessing();
    void pipelinedRequests();
    void malformedRequests();
    void registrationCanBeClosed();

    void sharedCalendarReachesTheMember();
    void readOnlyMemberIsRefused();
    void changedAccessReachesTheMember();
    void editingADeletedRecordAsksTheUser();
    void anOfflineEditIsNotLostToALaterDeletion();
    void restoringADeletedRecordIsLetThrough();
    void leavingTakesTheCopiesAlong();
    void linkRulesAreKept();

    void exchangesAreCounted();
    void copyIsConsistentAndSingleFile();
    void importMovesTheOldBaseOnce();
    void newerBaseIsRefused();
    void olderBaseStartsTheHistoryFromSessions();

private:
    struct Reply
    {
        int status = 0;
        QJsonObject body;
    };

    Reply call(const QByteArray &method, const QString &path, const QJsonObject &body = {},
               const QString &token = QString());
    QString registerUser(const QString &login, const QString &password = QStringLiteral("secret1"));
    static QJsonObject change(const QString &kind, const QString &id, qint64 modified,
                              const QJsonObject &data, bool deleted = false);
    QByteArray raw(const QByteArray &request, int expectedResponses = 1);

    /*! Puts one record of @p kind into @p calendar and returns the sync answer. */
    Reply put(const QString &token, const QString &kind, const QString &id,
              const QString &calendar, const QString &title, qint64 modified);

    /*! The share link of @p calendar, as the owner would copy it out of the card. */
    QString shareLink(const QString &token, const QString &calendar, bool singleUse = true,
                      qint64 lifeSeconds = 3600);

    /*! Everything @p token has, by "kind/id". */
    QHash<QString, QJsonObject> pull(const QString &token, qint64 since = 0);

    /*! The cursor the last pull() came back with. */
    qint64 m_cursor = 0;

    QTemporaryDir *m_dir = nullptr;
    Storage *m_storage = nullptr;
    Api *m_api = nullptr;
    HttpServer *m_server = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    ServerConfig m_config;
};

void ServerTest::init()
{
    m_dir = new QTemporaryDir;
    QVERIFY(m_dir->isValid());

    m_config = ServerConfig();
    m_config.dataDir = m_dir->path();
    m_config.passwordIterations = 1000;   // the real count would make the suite slow

    m_storage = new Storage;
    QString error;
    QVERIFY2(m_storage->open(m_config.databasePath(), &error), qPrintable(error));

    m_api = new Api(m_storage, m_config);
    m_server = new HttpServer;
    m_server->setMaxBodySize(1024 * 1024);
    m_server->setHandler([this](const HttpRequest &request) { return m_api->handle(request); });
    QVERIFY(m_server->listen(QHostAddress::LocalHost, 0));

    m_network = new QNetworkAccessManager;
}

void ServerTest::cleanup()
{
    delete m_network;
    delete m_server;
    delete m_api;
    delete m_storage;
    delete m_dir;
    m_network = nullptr;
    m_server = nullptr;
    m_api = nullptr;
    m_storage = nullptr;
    m_dir = nullptr;
}

ServerTest::Reply ServerTest::call(const QByteArray &method, const QString &path,
                                   const QJsonObject &body, const QString &token)
{
    QNetworkRequest request(
        QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_server->serverPort()).arg(path)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!token.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + token.toLatin1());

    const QByteArray payload = method == "GET" ? QByteArray()
                                               : QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_network->sendCustomRequest(request, method, payload);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();

    Reply result;
    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();
    return result;
}

QString ServerTest::registerUser(const QString &login, const QString &password)
{
    QJsonObject body;
    body.insert(QStringLiteral("login"), login);
    body.insert(QStringLiteral("password"), password);
    body.insert(QStringLiteral("device"), QStringLiteral("test"));
    const Reply reply = call("POST", QStringLiteral("/api/register"), body);
    return reply.status == 201 ? reply.body.value(QStringLiteral("token")).toString() : QString();
}

QJsonObject ServerTest::change(const QString &kind, const QString &id, qint64 modified,
                               const QJsonObject &data, bool deleted)
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), kind);
    object.insert(QStringLiteral("id"), id);
    object.insert(QStringLiteral("modified"), double(modified));
    object.insert(QStringLiteral("deleted"), deleted);
    if (!deleted)
        object.insert(QStringLiteral("data"), data);
    return object;
}

QByteArray ServerTest::raw(const QByteArray &request, int expectedResponses)
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, m_server->serverPort());
    if (!socket.waitForConnected(5000))
        return QByteArray();
    socket.write(request);

    QByteArray received;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        if (socket.bytesAvailable() || socket.waitForReadyRead(50))
            received += socket.readAll();
        if (received.count("HTTP/1.1 ") >= expectedResponses
            && received.endsWith("}"))
            break;
        if (socket.state() == QAbstractSocket::UnconnectedState)
            break;
    }
    return received;
}

// ---------------------------------------------------------------------------

void ServerTest::pingAnswers()
{
    const Reply reply = call("GET", QStringLiteral("/api/ping"));
    QCOMPARE(reply.status, 200);
    QCOMPARE(reply.body.value(QStringLiteral("service")).toString(),
             QStringLiteral("calendar-planner-server"));
    QCOMPARE(reply.body.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QCOMPARE(reply.body.value(QStringLiteral("api")).toInt(), Api::apiVersion());
}

void ServerTest::registerAndLogin()
{
    const QString token = registerUser(QStringLiteral("Anna"));
    QVERIFY(!token.isEmpty());

    // Logins are unique regardless of case.
    QJsonObject again;
    again.insert(QStringLiteral("login"), QStringLiteral("anna"));
    again.insert(QStringLiteral("password"), QStringLiteral("another1"));
    QCOMPARE(call("POST", QStringLiteral("/api/register"), again).status, 409);

    QJsonObject credentials;
    credentials.insert(QStringLiteral("login"), QStringLiteral("ANNA"));
    credentials.insert(QStringLiteral("password"), QStringLiteral("secret1"));
    const Reply login = call("POST", QStringLiteral("/api/login"), credentials);
    QCOMPARE(login.status, 200);
    const QString second = login.body.value(QStringLiteral("token")).toString();
    QVERIFY(!second.isEmpty());
    QVERIFY(second != token);
    QCOMPARE(login.body.value(QStringLiteral("user")).toObject().value(QStringLiteral("login")),
             QJsonValue(QStringLiteral("Anna")));

    const Reply me = call("GET", QStringLiteral("/api/me"), {}, second);
    QCOMPARE(me.status, 200);
    QCOMPARE(me.body.value(QStringLiteral("sessions")).toInt(), 2);
}

void ServerTest::rejectsBadCredentialsAndInput()
{
    registerUser(QStringLiteral("boris"));

    QJsonObject wrong;
    wrong.insert(QStringLiteral("login"), QStringLiteral("boris"));
    wrong.insert(QStringLiteral("password"), QStringLiteral("nope123"));
    QCOMPARE(call("POST", QStringLiteral("/api/login"), wrong).status, 401);

    QJsonObject shortPassword;
    shortPassword.insert(QStringLiteral("login"), QStringLiteral("vera"));
    shortPassword.insert(QStringLiteral("password"), QStringLiteral("123"));
    const Reply reply = call("POST", QStringLiteral("/api/register"), shortPassword);
    QCOMPARE(reply.status, 400);
    QVERIFY(!reply.body.value(QStringLiteral("message")).toString().isEmpty());

    QJsonObject badLogin;
    badLogin.insert(QStringLiteral("login"), QStringLiteral("имя с пробелом"));
    badLogin.insert(QStringLiteral("password"), QStringLiteral("secret1"));
    QCOMPARE(call("POST", QStringLiteral("/api/register"), badLogin).status, 400);
}

void ServerTest::requiresToken()
{
    QCOMPARE(call("POST", QStringLiteral("/api/sync"), QJsonObject()).status, 401);
    QCOMPARE(call("GET", QStringLiteral("/api/me"), {}, QStringLiteral("forged")).status, 401);
    QCOMPARE(call("GET", QStringLiteral("/api/nothing")).status, 401);
    QCOMPARE(call("GET", QStringLiteral("/elsewhere")).status, 404);
}

void ServerTest::syncRoundTrip()
{
    const QString token = registerUser(QStringLiteral("gleb"));

    QJsonObject event;
    event.insert(QStringLiteral("title"), QStringLiteral("Планёрка"));
    QJsonObject task;
    task.insert(QStringLiteral("title"), QStringLiteral("Купить хлеб"));

    QJsonObject push;
    push.insert(QStringLiteral("since"), 0);
    push.insert(QStringLiteral("changes"),
                QJsonArray { change(QStringLiteral("event"), QStringLiteral("e1"), 1000, event),
                             change(QStringLiteral("task"), QStringLiteral("t1"), 1000, task) });
    const Reply first = call("POST", QStringLiteral("/api/sync"), push, token);
    QCOMPARE(first.status, 200);
    QCOMPARE(first.body.value(QStringLiteral("accepted")).toInt(), 2);
    // The device has what it sent, nothing comes back.
    QVERIFY(first.body.value(QStringLiteral("changes")).toArray().isEmpty());
    const qint64 cursor = qint64(first.body.value(QStringLiteral("revision")).toDouble());
    QCOMPARE(cursor, 2);

    // A second device starts from scratch and gets everything.
    QJsonObject pull;
    pull.insert(QStringLiteral("since"), 0);
    const Reply second = call("POST", QStringLiteral("/api/sync"), pull, token);
    const QJsonArray changes = second.body.value(QStringLiteral("changes")).toArray();
    QCOMPARE(changes.size(), 2);
    QCOMPARE(changes.at(0).toObject().value(QStringLiteral("data")).toObject()
                 .value(QStringLiteral("title")).toString(),
             QStringLiteral("Планёрка"));

    // Nothing new since the cursor.
    QJsonObject idle;
    idle.insert(QStringLiteral("since"), double(cursor));
    const Reply third = call("POST", QStringLiteral("/api/sync"), idle, token);
    QVERIFY(third.body.value(QStringLiteral("changes")).toArray().isEmpty());
    QCOMPARE(qint64(third.body.value(QStringLiteral("revision")).toDouble()), cursor);

    // Another account sees none of it.
    const QString stranger = registerUser(QStringLiteral("stranger"));
    const Reply foreign = call("POST", QStringLiteral("/api/sync"), pull, stranger);
    QVERIFY(foreign.body.value(QStringLiteral("changes")).toArray().isEmpty());
}

void ServerTest::oneRecordIsAnsweredAlone()
{
    const QString token = registerUser(QStringLiteral("vera"));

    QJsonObject task;
    task.insert(QStringLiteral("title"), QStringLiteral("Позвонить"));
    QJsonObject push;
    push.insert(QStringLiteral("since"), 0);
    push.insert(QStringLiteral("changes"),
                QJsonArray { change(QStringLiteral("task"), QStringLiteral("t1"), 1500, task),
                             change(QStringLiteral("event"), QStringLiteral("e1"), 1500,
                                    QJsonObject()) });
    QCOMPARE(call("POST", QStringLiteral("/api/sync"), push, token).status, 200);

    // The record and nothing else, with the stamp the sync would have sent.
    const Reply found = call("GET", QStringLiteral("/api/record?kind=task&id=t1"), QJsonObject(),
                             token);
    QCOMPARE(found.status, 200);
    QVERIFY(found.body.value(QStringLiteral("found")).toBool());
    const QJsonObject record = found.body.value(QStringLiteral("change")).toObject();
    QCOMPARE(record.value(QStringLiteral("id")).toString(), QStringLiteral("t1"));
    QCOMPARE(qint64(record.value(QStringLiteral("modified")).toDouble()), qint64(1500));
    QCOMPARE(record.value(QStringLiteral("data")).toObject().value(QStringLiteral("title"))
                 .toString(),
             QStringLiteral("Позвонить"));

    // Unknown here, and unknown to another account.
    const Reply missing = call("GET", QStringLiteral("/api/record?kind=task&id=nope"),
                               QJsonObject(), token);
    QCOMPARE(missing.status, 200);
    QVERIFY(!missing.body.value(QStringLiteral("found")).toBool());
    const QString stranger = registerUser(QStringLiteral("stranger2"));
    const Reply foreign = call("GET", QStringLiteral("/api/record?kind=task&id=t1"),
                               QJsonObject(), stranger);
    QVERIFY(!foreign.body.value(QStringLiteral("found")).toBool());

    // A kind no record can have is a bad request, not an empty answer.
    QCOMPARE(call("GET", QStringLiteral("/api/record?kind=Task!&id=t1"), QJsonObject(), token)
                 .status,
             400);
}

void ServerTest::newestChangeWins()
{
    const QString token = registerUser(QStringLiteral("dina"));

    QJsonObject later;
    later.insert(QStringLiteral("title"), QStringLiteral("с телефона"));
    QJsonObject push;
    push.insert(QStringLiteral("since"), 0);
    push.insert(QStringLiteral("changes"),
                QJsonArray { change(QStringLiteral("task"), QStringLiteral("t"), 2000, later) });
    const Reply phone = call("POST", QStringLiteral("/api/sync"), push, token);
    QCOMPARE(phone.body.value(QStringLiteral("accepted")).toInt(), 1);

    // The desktop edited the same task earlier but syncs only now: it loses and
    // gets the phone's version back even though its cursor is already past it.
    QJsonObject earlier;
    earlier.insert(QStringLiteral("title"), QStringLiteral("с компьютера"));
    QJsonObject stale;
    stale.insert(QStringLiteral("since"), 5);
    stale.insert(QStringLiteral("changes"),
                 QJsonArray { change(QStringLiteral("task"), QStringLiteral("t"), 1500, earlier) });
    const Reply desktop = call("POST", QStringLiteral("/api/sync"), stale, token);
    QCOMPARE(desktop.body.value(QStringLiteral("rejected")).toInt(), 1);
    const QJsonArray back = desktop.body.value(QStringLiteral("changes")).toArray();
    QCOMPARE(back.size(), 1);
    QCOMPARE(back.at(0).toObject().value(QStringLiteral("data")).toObject()
                 .value(QStringLiteral("title")).toString(),
             QStringLiteral("с телефона"));

    // A newer edit from the desktop replaces it.
    QJsonObject newest;
    newest.insert(QStringLiteral("since"), 1);
    newest.insert(QStringLiteral("changes"),
                  QJsonArray { change(QStringLiteral("task"), QStringLiteral("t"), 3000, earlier) });
    QCOMPARE(call("POST", QStringLiteral("/api/sync"), newest, token)
                 .body.value(QStringLiteral("accepted")).toInt(),
             1);

    QJsonObject check;
    check.insert(QStringLiteral("since"), 0);
    const QJsonArray all = call("POST", QStringLiteral("/api/sync"), check, token)
                               .body.value(QStringLiteral("changes")).toArray();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.at(0).toObject().value(QStringLiteral("data")).toObject()
                 .value(QStringLiteral("title")).toString(),
             QStringLiteral("с компьютера"));
}

void ServerTest::deletionsTravel()
{
    const QString token = registerUser(QStringLiteral("egor"));

    QJsonObject data;
    data.insert(QStringLiteral("title"), QStringLiteral("x"));
    QJsonObject push;
    push.insert(QStringLiteral("since"), 0);
    push.insert(QStringLiteral("changes"),
                QJsonArray { change(QStringLiteral("event"), QStringLiteral("a"), 100, data),
                             change(QStringLiteral("event"), QStringLiteral("b"), 100, data) });
    call("POST", QStringLiteral("/api/sync"), push, token);

    QJsonObject removal;
    removal.insert(QStringLiteral("since"), 2);
    removal.insert(QStringLiteral("changes"),
                   QJsonArray { change(QStringLiteral("event"), QStringLiteral("a"), 200, {}, true) });
    call("POST", QStringLiteral("/api/sync"), removal, token);

    // A device that saw both records learns about the deletion...
    QJsonObject known;
    known.insert(QStringLiteral("since"), 2);
    const QJsonArray tombstones = call("POST", QStringLiteral("/api/sync"), known, token)
                                      .body.value(QStringLiteral("changes")).toArray();
    QCOMPARE(tombstones.size(), 1);
    QVERIFY(tombstones.at(0).toObject().value(QStringLiteral("deleted")).toBool());
    QVERIFY(!tombstones.at(0).toObject().contains(QStringLiteral("data")));

    // ...a new one simply never hears of the deleted record.
    QJsonObject fresh;
    fresh.insert(QStringLiteral("since"), 0);
    const QJsonArray all = call("POST", QStringLiteral("/api/sync"), fresh, token)
                               .body.value(QStringLiteral("changes")).toArray();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("b"));
}

void ServerTest::logoutEndsSession()
{
    const QString token = registerUser(QStringLiteral("fedor"));
    QCOMPARE(call("GET", QStringLiteral("/api/me"), {}, token).status, 200);
    QCOMPARE(call("POST", QStringLiteral("/api/logout"), {}, token).status, 200);
    QCOMPARE(call("GET", QStringLiteral("/api/me"), {}, token).status, 401);
}

void ServerTest::passwordChangeLogsOutOthers()
{
    const QString first = registerUser(QStringLiteral("galina"), QStringLiteral("secret1"));
    QJsonObject credentials;
    credentials.insert(QStringLiteral("login"), QStringLiteral("galina"));
    credentials.insert(QStringLiteral("password"), QStringLiteral("secret1"));
    const QString second = call("POST", QStringLiteral("/api/login"), credentials)
                               .body.value(QStringLiteral("token")).toString();

    QJsonObject change;
    change.insert(QStringLiteral("oldPassword"), QStringLiteral("wrong00"));
    change.insert(QStringLiteral("newPassword"), QStringLiteral("better22"));
    QCOMPARE(call("POST", QStringLiteral("/api/password"), change, second).status, 403);

    change.insert(QStringLiteral("oldPassword"), QStringLiteral("secret1"));
    QCOMPARE(call("POST", QStringLiteral("/api/password"), change, second).status, 200);
    QCOMPARE(call("GET", QStringLiteral("/api/me"), {}, second).status, 200);
    QCOMPARE(call("GET", QStringLiteral("/api/me"), {}, first).status, 401);

    credentials.insert(QStringLiteral("password"), QStringLiteral("better22"));
    QCOMPARE(call("POST", QStringLiteral("/api/login"), credentials).status, 200);
}

void ServerTest::throttlesGuessing()
{
    registerUser(QStringLiteral("hanna"));
    QJsonObject wrong;
    wrong.insert(QStringLiteral("login"), QStringLiteral("hanna"));
    wrong.insert(QStringLiteral("password"), QStringLiteral("guess00"));
    for (int i = 0; i < 10; ++i)
        QCOMPARE(call("POST", QStringLiteral("/api/login"), wrong).status, 401);

    // Even the right password is refused for a while.
    wrong.insert(QStringLiteral("password"), QStringLiteral("secret1"));
    QCOMPARE(call("POST", QStringLiteral("/api/login"), wrong).status, 429);
}

void ServerTest::pipelinedRequests()
{
    const QByteArray two = "GET /api/ping HTTP/1.1\r\nHost: x\r\n\r\n"
                           "GET /api/ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    const QByteArray answer = raw(two, 2);
    QCOMPARE(answer.count("HTTP/1.1 200 OK"), 2);
    QVERIFY(answer.contains("Connection: close"));
}

void ServerTest::malformedRequests()
{
    QVERIFY(raw("NONSENSE\r\n\r\n").startsWith("HTTP/1.1 400"));
    QVERIFY(raw("POST /api/login HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n")
                .startsWith("HTTP/1.1 501"));
    QVERIFY(raw("POST /api/login HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n")
                .startsWith("HTTP/1.1 413"));

    const QByteArray notJson = "POST /api/login HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc";
    QVERIFY(raw(notJson).startsWith("HTTP/1.1 400"));
}

void ServerTest::registrationCanBeClosed()
{
    ServerConfig closed = m_config;
    closed.allowRegistration = false;
    Api api(m_storage, closed);
    m_server->setHandler([&api](const HttpRequest &request) { return api.handle(request); });

    QJsonObject body;
    body.insert(QStringLiteral("login"), QStringLiteral("ivan"));
    body.insert(QStringLiteral("password"), QStringLiteral("secret1"));
    QCOMPARE(call("POST", QStringLiteral("/api/register"), body).status, 403);
    QVERIFY(!call("GET", QStringLiteral("/api/ping")).body.value(QStringLiteral("registration"))
                 .toBool(true));

    m_server->setHandler([this](const HttpRequest &request) { return m_api->handle(request); });
}

QTEST_GUILESS_MAIN(ServerTest)
#include "tst_server.moc"

// ------------------------------------------------------- shared calendars ----

ServerTest::Reply ServerTest::put(const QString &token, const QString &kind, const QString &id,
                                  const QString &calendar, const QString &title, qint64 modified)
{
    QJsonObject data;
    data.insert(QStringLiteral("id"), id);
    // A calendar carries a name, everything else a title: the planner spells
    // them that way and the peek hands the calendar to the add dialog as is.
    data.insert(kind == QLatin1String("calendar") ? QStringLiteral("name")
                                                  : QStringLiteral("title"),
                title);
    if (!calendar.isEmpty())
        data.insert(QStringLiteral("calendarId"), calendar);

    QJsonArray changes;
    changes.append(change(kind, id, modified, data));
    QJsonObject body;
    body.insert(QStringLiteral("since"), 0);
    body.insert(QStringLiteral("changes"), changes);
    return call("POST", QStringLiteral("/api/sync"), body, token);
}

QString ServerTest::shareLink(const QString &token, const QString &calendar, bool singleUse,
                              qint64 lifeSeconds)
{
    QJsonObject body;
    body.insert(QStringLiteral("calendar"), calendar);
    body.insert(QStringLiteral("singleUse"), singleUse);
    body.insert(QStringLiteral("lifeSeconds"), double(lifeSeconds));
    const Reply reply = call("POST", QStringLiteral("/api/share/link"), body, token);
    return reply.body.value(QStringLiteral("token")).toString();
}

QHash<QString, QJsonObject> ServerTest::pull(const QString &token, qint64 since)
{
    QJsonObject body;
    body.insert(QStringLiteral("since"), double(since));
    body.insert(QStringLiteral("changes"), QJsonArray());
    const Reply reply = call("POST", QStringLiteral("/api/sync"), body, token);

    m_cursor = qint64(reply.body.value(QStringLiteral("revision")).toDouble(0));

    QHash<QString, QJsonObject> result;
    const QJsonArray changes = reply.body.value(QStringLiteral("changes")).toArray();
    for (const QJsonValue &value : changes) {
        const QJsonObject item = value.toObject();
        result.insert(item.value(QStringLiteral("kind")).toString() + QLatin1Char('/')
                          + item.value(QStringLiteral("id")).toString(),
                      item);
    }
    return result;
}

/*!
 * The whole way of a shared calendar: the owner opens it, hands out a link, and
 * what the calendar holds turns up in the member's own records - under the
 * member's own calendar id, because the twin is a calendar of their planner.
 */
void ServerTest::sharedCalendarReachesTheMember()
{
    const QString anna = registerUser(QStringLiteral("anna"));
    const QString boris = registerUser(QStringLiteral("boris"));
    QVERIFY(!anna.isEmpty());
    QVERIFY(!boris.isEmpty());

    // Anna has a calendar with one event in it.
    QCOMPARE(put(anna, QStringLiteral("calendar"), QStringLiteral("cal-a"), QString(),
                 QStringLiteral("Семья"), 1000)
                 .status,
             200);
    QCOMPARE(put(anna, QStringLiteral("event"), QStringLiteral("ev-1"), QStringLiteral("cal-a"),
                 QStringLiteral("Ужин"), 1001)
                 .status,
             200);

    QJsonObject config;
    config.insert(QStringLiteral("calendar"), QStringLiteral("cal-a"));
    config.insert(QStringLiteral("enabled"), true);
    config.insert(QStringLiteral("mode"), QStringLiteral("custom"));
    const Reply configured = call("POST", QStringLiteral("/api/share"), config, anna);
    QCOMPARE(configured.status, 200);
    QCOMPARE(configured.body.value(QStringLiteral("share")).toObject()
                 .value(QStringLiteral("mode")).toString(),
             QStringLiteral("custom"));

    const QString token = shareLink(anna, QStringLiteral("cal-a"));
    QVERIFY(!token.isEmpty());

    // Before joining, the add dialog asks what the link leads to.
    const Reply peek = call("GET", QStringLiteral("/api/share/peek?token=") + token, {}, boris);
    QCOMPARE(peek.status, 200);
    QCOMPARE(peek.body.value(QStringLiteral("calendar")).toObject()
                 .value(QStringLiteral("name")).toString(),
             QStringLiteral("Семья"));

    QJsonObject join;
    join.insert(QStringLiteral("token"), token);
    join.insert(QStringLiteral("twinId"), QStringLiteral("twin-b"));
    const Reply joined = call("POST", QStringLiteral("/api/share/join"), join, boris);
    QCOMPARE(joined.status, 201);
    QCOMPARE(joined.body.value(QStringLiteral("access")).toString(), QStringLiteral("read"));

    // The event is now a record of Boris, in the calendar Boris calls it by.
    const QHash<QString, QJsonObject> mine = pull(boris);
    QVERIFY(mine.contains(QStringLiteral("event/ev-1")));
    QCOMPARE(mine.value(QStringLiteral("event/ev-1")).value(QStringLiteral("data")).toObject()
                 .value(QStringLiteral("calendarId")).toString(),
             QStringLiteral("twin-b"));

    // Anna's own calendar record never travels: its name and colour are hers.
    QVERIFY(!mine.contains(QStringLiteral("calendar/cal-a")));

    // The owner sees who came in.
    const Reply list = call("GET", QStringLiteral("/api/share?calendar=cal-a"), {}, anna);
    const QJsonArray members =
        list.body.value(QStringLiteral("share")).toObject()
            .value(QStringLiteral("members")).toArray();
    QCOMPARE(members.size(), 1);
    QCOMPARE(members.first().toObject().value(QStringLiteral("login")).toString(),
             QStringLiteral("boris"));
}

/*!
 * Read access really is read only, and raising the member in the custom mode
 * opens the way back to the owner's calendar.
 */
void ServerTest::readOnlyMemberIsRefused()
{
    const QString anna = registerUser(QStringLiteral("anna"));
    const QString boris = registerUser(QStringLiteral("boris"));

    put(anna, QStringLiteral("calendar"), QStringLiteral("cal-a"), QString(),
        QStringLiteral("Семья"), 1000);
    put(anna, QStringLiteral("event"), QStringLiteral("ev-1"), QStringLiteral("cal-a"),
        QStringLiteral("Ужин"), 1001);

    QJsonObject config;
    config.insert(QStringLiteral("calendar"), QStringLiteral("cal-a"));
    config.insert(QStringLiteral("enabled"), true);
    config.insert(QStringLiteral("mode"), QStringLiteral("custom"));
    call("POST", QStringLiteral("/api/share"), config, anna);

    QJsonObject join;
    join.insert(QStringLiteral("token"), shareLink(anna, QStringLiteral("cal-a")));
    join.insert(QStringLiteral("twinId"), QStringLiteral("twin-b"));
    QCOMPARE(call("POST", QStringLiteral("/api/share/join"), join, boris).status, 201);

    // Reading is all Boris may do, so the change comes back as a conflict.
    const Reply refused = put(boris, QStringLiteral("event"), QStringLiteral("ev-1"),
                              QStringLiteral("twin-b"), QStringLiteral("Не ужин"), 2000);
    QCOMPARE(refused.status, 200);
    const QJsonArray conflicts = refused.body.value(QStringLiteral("conflicts")).toArray();
    QCOMPARE(conflicts.size(), 1);
    QCOMPARE(conflicts.first().toObject().value(QStringLiteral("reason")).toString(),
             QStringLiteral("read_only"));

    // Anna still has her own title.
    QCOMPARE(pull(anna).value(QStringLiteral("event/ev-1")).value(QStringLiteral("data"))
                 .toObject().value(QStringLiteral("title")).toString(),
             QStringLiteral("Ужин"));

    // The owner raises Boris; now the same change goes through and reaches her.
    QJsonObject raise;
    raise.insert(QStringLiteral("calendar"), QStringLiteral("cal-a"));
    raise.insert(QStringLiteral("login"), QStringLiteral("boris"));
    raise.insert(QStringLiteral("access"), QStringLiteral("edit"));
    QCOMPARE(call("POST", QStringLiteral("/api/share/member"), raise, anna).status, 200);

    const Reply accepted = put(boris, QStringLiteral("event"), QStringLiteral("ev-1"),
                               QStringLiteral("twin-b"), QStringLiteral("Не ужин"), 2001);
    QCOMPARE(accepted.body.value(QStringLiteral("conflicts")).toArray().size(), 0);

    const QJsonObject annas = pull(anna).value(QStringLiteral("event/ev-1"))
                                  .value(QStringLiteral("data")).toObject();
    QCOMPARE(annas.value(QStringLiteral("title")).toString(), QStringLiteral("Не ужин"));
    // It came back into her calendar, not into the one Boris calls it by.
    QCOMPARE(annas.value(QStringLiteral("calendarId")).toString(), QStringLiteral("cal-a"));
}

/*!
 * A right the owner changes has to reach the member's planner, and the only
 * thing that planner ever sees is its own records - so the twin calendar is
 * where it has to land. Without this the member is raised on the server and
 * goes on believing the calendar is read only: their editor will not even
 * offer it.
 */
void ServerTest::changedAccessReachesTheMember()
{
    const QString anna = registerUser(QStringLiteral("anna"));
    const QString boris = registerUser(QStringLiteral("boris"));

    put(anna, QStringLiteral("calendar"), QStringLiteral("cal-a"), QString(),
        QStringLiteral("Семья"), 1000);

    QJsonObject config;
    config.insert(QStringLiteral("calendar"), QStringLiteral("cal-a"));
    config.insert(QStringLiteral("enabled"), true);
    config.insert(QStringLiteral("mode"), QStringLiteral("custom"));
    call("POST", QStringLiteral("/api/share"), config, anna);

    QJsonObject join;
    join.insert(QStringLiteral("token"), shareLink(anna, QStringLiteral("cal-a")));
    join.insert(QStringLiteral("twinId"), QStringLiteral("twin-b"));
    QCOMPARE(call("POST", QStringLiteral("/api/share/join"), join, boris).status, 201);

    // The twin as the member's own planner keeps it: their name, their colour.
    put(boris, QStringLiteral("calendar"), QStringLiteral("twin-b"), QString(),
        QStringLiteral("Аннин"), 2000);

    QJsonObject raise;
    raise.insert(QStringLiteral("calendar"), QStringLiteral("cal-a"));
    raise.insert(QStringLiteral("login"), QStringLiteral("boris"));
    raise.insert(QStringLiteral("access"), QStringLiteral("edit"));
    QCOMPARE(call("POST", QStringLiteral("/api/share/member"), raise, anna).status, 200);

    QJsonObject twin = pull(boris).value(QStringLiteral("calendar/twin-b"))
                           .value(QStringLiteral("data")).toObject();
    QCOMPARE(twin.value(QStringLiteral("access")).toString(), QStringLiteral("edit"));
    // The raise carries the right and nothing else: the name stays the member's.
    QCOMPARE(twin.value(QStringLiteral("name")).toString(), QStringLiteral("Аннин"));

    // Lowering the whole calendar to read has to travel the same way.
    config.insert(QStringLiteral("mode"), QStringLiteral("read"));
    QCOMPARE(call("POST", QStringLiteral("/api/share"), config, anna).status, 200);

    twin = pull(boris).value(QStringLiteral("calendar/twin-b"))
               .value(QStringLiteral("data")).toObject();
    QCOMPARE(twin.value(QStringLiteral("access")).toString(), QStringLiteral("read"));
}

/*!
 * The case the planner has to ask about: the owner deleted the record while the
 * member was editing it. The change is not applied silently either way - the
 * member is told, and offered to bring it back.
 */
void ServerTest::editingADeletedRecordAsksTheUser()
{
    const QString anna = registerUser(QStringLiteral("anna"));
    const QString boris = registerUser(QStringLiteral("boris"));

    put(anna, QStringLiteral("calendar"), QStringLiteral("cal-a"), QString(),
        QStringLiteral("Семья"), 1000);
    put(anna, QStringLiteral("event"), QStringLiteral("ev-1"), QStringLiteral("cal-a"),
        QStringLiteral("Ужин"), 1001);

    QJsonObject config;
    config.insert(QStringLiteral("calendar"), QStringLiteral("cal-a"));
    config.insert(QStringLiteral("enabled"), true);
    config.insert(QStringLiteral("mode"), QStringLiteral("edit"));
    call("POST", QStringLiteral("/api/share"), config, anna);

    QJsonObject join;
    join.insert(QStringLiteral("token"), shareLink(anna, QStringLiteral("cal-a")));
    join.insert(QStringLiteral("twinId"), QStringLiteral("twin-b"));
    const Reply joined = call("POST", QStringLiteral("/api/share/join"), join, boris);
    QCOMPARE(joined.body.value(QStringLiteral("access")).toString(), QStringLiteral("edit"));

    // Anna deletes the event.
    QJsonArray removal;
    removal.append(change(QStringLiteral("event"), QStringLiteral("ev-1"), 3000, QJsonObject(),
                          true));
    QJsonObject body;
    body.insert(QStringLiteral("since"), 0);
    body.insert(QStringLiteral("changes"), removal);
    QCOMPARE(call("POST", QStringLiteral("/api/sync"), body, anna).status, 200);

    // Boris, who had it open, saves his edit into a record that is gone.
    const Reply late = put(boris, QStringLiteral("event"), QStringLiteral("ev-1"),
                           QStringLiteral("twin-b"), QStringLiteral("Поздняя правка"), 4000);
    const QJsonArray conflicts = late.body.value(QStringLiteral("conflicts")).toArray();
    QCOMPARE(conflicts.size(), 1);

    const QJsonObject conflict = conflicts.first().toObject();
    QCOMPARE(conflict.value(QStringLiteral("reason")).toString(), QStringLiteral("deleted"));
    QCOMPARE(conflict.value(QStringLiteral("calendarId")).toString(), QStringLiteral("twin-b"));
    // What he tried to save comes back with it, so "вернуть" has something to restore.
    QCOMPARE(conflict.value(QStringLiteral("data")).toObject()
                 .value(QStringLiteral("title")).toString(),
             QStringLiteral("Поздняя правка"));
}

/*!
 * The realistic shape of that case: the member was offline while editing, so
 * the owner's deletion is the *later* change of the two. Plain "newest wins"
 * would drop the member's work without a word - the one thing this whole
 * conflict business exists to prevent - so the question is asked here too,
 * whichever of the two happened last.
 */
void ServerTest::anOfflineEditIsNotLostToALaterDeletion()
{
    const QString anna = registerUser(QStringLiteral("anna"));
    const QString boris = registerUser(QStringLiteral("boris"));

    put(anna, QStringLiteral("calendar"), QStringLiteral("cal-a"), QString(),
        QStringLiteral("Семья"), 1000);
    put(anna, QStringLiteral("event"), QStringLiteral("ev-1"), QStringLiteral("cal-a"),
        QStringLiteral("Ужин"), 1001);

    QJsonObject config;
    config.insert(QStringLiteral("calendar"), QStringLiteral("cal-a"));
    config.insert(QStringLiteral("enabled"), true);
    config.insert(QStringLiteral("mode"), QStringLiteral("edit"));
    call("POST", QStringLiteral("/api/share"), config, anna);

    QJsonObject join;
    join.insert(QStringLiteral("token"), shareLink(anna, QStringLiteral("cal-a")));
    join.insert(QStringLiteral("twinId"), QStringLiteral("twin-b"));
    call("POST", QStringLiteral("/api/share/join"), join, boris);

    // Anna deletes at 3000 and her change reaches the server first, tombstoning
    // the copy in Boris's records as well. His edit was made at 2000, with no
    // network to send it on.
    QJsonArray removal;
    removal.append(change(QStringLiteral("event"), QStringLiteral("ev-1"), 3000, QJsonObject(),
                          true));
    QJsonObject deletion;
    deletion.insert(QStringLiteral("since"), 0);
    deletion.insert(QStringLiteral("changes"), removal);
    call("POST", QStringLiteral("/api/sync"), deletion, anna);

    const Reply late = put(boris, QStringLiteral("event"), QStringLiteral("ev-1"),
                           QStringLiteral("twin-b"), QStringLiteral("Правка вслепую"), 2000);
    const QJsonArray conflicts = late.body.value(QStringLiteral("conflicts")).toArray();
    QCOMPARE(conflicts.size(), 1);
    const QJsonObject conflict = conflicts.first().toObject();
    QCOMPARE(conflict.value(QStringLiteral("reason")).toString(), QStringLiteral("deleted"));
    // The change comes back with it, so the planner can offer to write it again.
    QCOMPARE(conflict.value(QStringLiteral("data")).toObject()
                 .value(QStringLiteral("title")).toString(),
             QStringLiteral("Правка вслепую"));
}

/*!
 * The answer to that question. "Вернуть" sends the same record again, marked
 * as a restore: without the mark the server would refuse it exactly as before
 * and the planner would ask forever.
 */
void ServerTest::restoringADeletedRecordIsLetThrough()
{
    const QString anna = registerUser(QStringLiteral("anna"));
    const QString boris = registerUser(QStringLiteral("boris"));

    put(anna, QStringLiteral("calendar"), QStringLiteral("cal-a"), QString(),
        QStringLiteral("Семья"), 1000);
    put(anna, QStringLiteral("event"), QStringLiteral("ev-1"), QStringLiteral("cal-a"),
        QStringLiteral("Ужин"), 1001);

    QJsonObject config;
    config.insert(QStringLiteral("calendar"), QStringLiteral("cal-a"));
    config.insert(QStringLiteral("enabled"), true);
    config.insert(QStringLiteral("mode"), QStringLiteral("edit"));
    call("POST", QStringLiteral("/api/share"), config, anna);

    QJsonObject join;
    join.insert(QStringLiteral("token"), shareLink(anna, QStringLiteral("cal-a")));
    join.insert(QStringLiteral("twinId"), QStringLiteral("twin-b"));
    call("POST", QStringLiteral("/api/share/join"), join, boris);

    QJsonArray removal;
    removal.append(change(QStringLiteral("event"), QStringLiteral("ev-1"), 3000, QJsonObject(),
                          true));
    QJsonObject deletion;
    deletion.insert(QStringLiteral("since"), 0);
    deletion.insert(QStringLiteral("changes"), removal);
    call("POST", QStringLiteral("/api/sync"), deletion, anna);

    // Refused once: this is the question the planner puts to the user.
    QCOMPARE(put(boris, QStringLiteral("event"), QStringLiteral("ev-1"),
                 QStringLiteral("twin-b"), QStringLiteral("Вернуть это"), 4000)
                 .body.value(QStringLiteral("conflicts")).toArray().size(),
             1);

    // The same record again, now marked: the user has seen the deletion.
    QJsonObject data;
    data.insert(QStringLiteral("id"), QStringLiteral("ev-1"));
    data.insert(QStringLiteral("title"), QStringLiteral("Вернуть это"));
    data.insert(QStringLiteral("calendarId"), QStringLiteral("twin-b"));
    QJsonObject restored = change(QStringLiteral("event"), QStringLiteral("ev-1"), 4001, data);
    restored.insert(QStringLiteral("restore"), true);

    QJsonArray changes;
    changes.append(restored);
    QJsonObject body;
    body.insert(QStringLiteral("since"), 0);
    body.insert(QStringLiteral("changes"), changes);
    const Reply second = call("POST", QStringLiteral("/api/sync"), body, boris);
    QCOMPARE(second.body.value(QStringLiteral("conflicts")).toArray().size(), 0);

    // It is back for the owner, in her calendar and with the member's title.
    const QJsonObject annas = pull(anna).value(QStringLiteral("event/ev-1"))
                                  .value(QStringLiteral("data")).toObject();
    QCOMPARE(annas.value(QStringLiteral("title")).toString(), QStringLiteral("Вернуть это"));
    QCOMPARE(annas.value(QStringLiteral("calendarId")).toString(), QStringLiteral("cal-a"));
}

/*!
 * Giving the access up takes the records off the member's device and takes the
 * member out of the owner's list: the same act seen from the two sides.
 */
void ServerTest::leavingTakesTheCopiesAlong()
{
    const QString anna = registerUser(QStringLiteral("anna"));
    const QString boris = registerUser(QStringLiteral("boris"));

    put(anna, QStringLiteral("calendar"), QStringLiteral("cal-a"), QString(),
        QStringLiteral("Семья"), 1000);
    put(anna, QStringLiteral("event"), QStringLiteral("ev-1"), QStringLiteral("cal-a"),
        QStringLiteral("Ужин"), 1001);

    QJsonObject config;
    config.insert(QStringLiteral("calendar"), QStringLiteral("cal-a"));
    config.insert(QStringLiteral("enabled"), true);
    config.insert(QStringLiteral("mode"), QStringLiteral("read"));
    call("POST", QStringLiteral("/api/share"), config, anna);

    QJsonObject join;
    join.insert(QStringLiteral("token"), shareLink(anna, QStringLiteral("cal-a")));
    join.insert(QStringLiteral("twinId"), QStringLiteral("twin-b"));
    call("POST", QStringLiteral("/api/share/join"), join, boris);
    QVERIFY(pull(boris).contains(QStringLiteral("event/ev-1")));
    const qint64 cursor = m_cursor;

    QJsonObject leave;
    leave.insert(QStringLiteral("twinId"), QStringLiteral("twin-b"));
    QCOMPARE(call("POST", QStringLiteral("/api/share/leave"), leave, boris).status, 200);

    // A tombstone, not silence: his devices have the record and must drop it.
    // It is asked for from the cursor he had, since a device starting from
    // scratch is never sent the tombstones of records it has never seen.
    const QJsonObject gone = pull(boris, cursor).value(QStringLiteral("event/ev-1"));
    QVERIFY(gone.value(QStringLiteral("deleted")).toBool());

    const Reply list = call("GET", QStringLiteral("/api/share?calendar=cal-a"), {}, anna);
    QCOMPARE(list.body.value(QStringLiteral("share")).toObject()
                 .value(QStringLiteral("members")).toArray().size(),
             0);
    // Anna keeps her own records untouched.
    QVERIFY(!pull(anna).value(QStringLiteral("event/ev-1"))
                 .value(QStringLiteral("deleted")).toBool());
}

/*! A one time link works once, and an expired one never. */
void ServerTest::linkRulesAreKept()
{
    const QString anna = registerUser(QStringLiteral("anna"));
    const QString boris = registerUser(QStringLiteral("boris"));
    const QString vera = registerUser(QStringLiteral("vera"));

    put(anna, QStringLiteral("calendar"), QStringLiteral("cal-a"), QString(),
        QStringLiteral("Семья"), 1000);

    QJsonObject config;
    config.insert(QStringLiteral("calendar"), QStringLiteral("cal-a"));
    config.insert(QStringLiteral("enabled"), true);
    config.insert(QStringLiteral("mode"), QStringLiteral("custom"));
    call("POST", QStringLiteral("/api/share"), config, anna);

    const QString once = shareLink(anna, QStringLiteral("cal-a"), true);
    QJsonObject join;
    join.insert(QStringLiteral("token"), once);
    join.insert(QStringLiteral("twinId"), QStringLiteral("twin-b"));
    QCOMPARE(call("POST", QStringLiteral("/api/share/join"), join, boris).status, 201);

    // The second one through the same link is turned away.
    QJsonObject second;
    second.insert(QStringLiteral("token"), once);
    second.insert(QStringLiteral("twinId"), QStringLiteral("twin-v"));
    QCOMPARE(call("POST", QStringLiteral("/api/share/join"), second, vera).status, 400);

    // A link that has already expired opens nothing either.
    const QString stale = shareLink(anna, QStringLiteral("cal-a"), false, 0);
    QVERIFY(!stale.isEmpty()); // an endless link is allowed, that is "бессрочная"

    // The owner's own link leads to the owner's own calendar, which is refused.
    const Reply own = call("GET", QStringLiteral("/api/share/peek?token=") + stale, {}, anna);
    QCOMPARE(own.status, 409);

    // A multi use link lets the second account in as well.
    QJsonObject third;
    third.insert(QStringLiteral("token"), stale);
    third.insert(QStringLiteral("twinId"), QStringLiteral("twin-v"));
    QCOMPARE(call("POST", QStringLiteral("/api/share/join"), third, vera).status, 201);
}

// ------------------------------------------------------ base and statistics ----

namespace {

/*! Every row of @p statement on a connection of its own to @p path. */
QList<QVariantList> rows(const QString &path, const QString &statement)
{
    QList<QVariantList> result;
    const QString name = QStringLiteral("test-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(statement)) {
                while (query.next()) {
                    QVariantList row;
                    for (int i = 0; i < query.record().count(); ++i)
                        row << query.value(i);
                    result << row;
                }
            }
        }
    }
    QSqlDatabase::removeDatabase(name);
    return result;
}

} // namespace

/*!
 * Every exchange is counted into the activity of its day - the source of the
 * active accounts and the charts of the admin panel.
 */
void ServerTest::exchangesAreCounted()
{
    const QString token = registerUser(QStringLiteral("dina"));
    QJsonObject data;
    data.insert(QStringLiteral("title"), QStringLiteral("Встреча"));
    QJsonObject push;
    push.insert(QStringLiteral("since"), 0);
    push.insert(QStringLiteral("changes"),
                QJsonArray { change(QStringLiteral("event"), QStringLiteral("e1"), 1000, data) });
    QCOMPARE(call("POST", QStringLiteral("/api/sync"), push, token).status, 200);
    QJsonObject pull;
    pull.insert(QStringLiteral("since"), 0);
    QCOMPARE(call("POST", QStringLiteral("/api/sync"), pull, token).status, 200);

    const QList<QVariantList> activity = rows(
        m_config.databasePath(),
        QStringLiteral("SELECT day, syncs, changes_in, changes_out, bytes_in, bytes_out, last_at"
                       " FROM activity"));
    QCOMPARE(activity.size(), 1);
    const QVariantList &day = activity.first();
    QCOMPARE(day.at(0).toString(), QDate::currentDate().toString(Qt::ISODate));
    QCOMPARE(day.at(1).toInt(), 2);
    QCOMPARE(day.at(2).toInt(), 1);   // the event went up once
    QCOMPARE(day.at(3).toInt(), 1);   // and came back to the device that started from 0
    QVERIFY(day.at(4).toLongLong() > 0);
    QVERIFY(day.at(5).toLongLong() > 0);
    const QDateTime last = QDateTime::fromString(day.at(6).toString(), Qt::ISODate);
    QVERIFY(qAbs(last.secsTo(QDateTime::currentDateTimeUtc())) < 60);
}

/*!
 * A copy taken while the server works holds what is still in its WAL, and it
 * is one file: a backup is carried around without a -wal beside it.
 */
void ServerTest::copyIsConsistentAndSingleFile()
{
    const QString token = registerUser(QStringLiteral("emil"));
    QCOMPARE(put(token, QStringLiteral("event"), QStringLiteral("e1"), QString(),
                 QStringLiteral("Раз"), 1000).status,
             200);

    const QString copy = m_dir->filePath(QStringLiteral("copies/copy.db"));
    QString error;
    QVERIFY2(Storage::copyDatabase(m_config.databasePath(), copy, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(copy + QStringLiteral("-wal")));

    const DatabaseInfo info = Storage::inspect(copy, true);
    QVERIFY2(info.ok, qPrintable(info.error));
    QCOMPARE(info.users, 1);
    QCOMPARE(info.records, 1);
    QCOMPARE(info.schemaVersion, Storage::schemaVersion());
    QCOMPARE(rows(copy, QStringLiteral("PRAGMA journal_mode")).value(0).value(0).toString(),
             QStringLiteral("delete"));

    // A copy never goes over an existing file.
    QVERIFY(!Storage::copyDatabase(m_config.databasePath(), copy, &error));
    QVERIFY(!Storage::inspect(m_dir->filePath(QStringLiteral("nothing.db"))).ok);
}

/*!
 * --import-db, the way the installer moves a console server's base to the
 * service: once, and never over a base that has accounts.
 */
void ServerTest::importMovesTheOldBaseOnce()
{
    const QString token = registerUser(QStringLiteral("fedor"));
    QCOMPARE(put(token, QStringLiteral("task"), QStringLiteral("t1"), QString(),
                 QStringLiteral("Купить хлеб"), 1000).status,
             200);
    const QString source = m_config.databasePath();
    const QString target = m_dir->filePath(QStringLiteral("service/server.db"));

    QString message;
    QCOMPARE(Storage::importDatabase(source, target, &message), Storage::ImportResult::Imported);
    QCOMPARE(Storage::inspect(target).users, 1);
    QCOMPARE(Storage::inspect(target).records, 1);

    // The service has accounts now: a second run does nothing.
    registerUser(QStringLiteral("late"));
    QCOMPARE(Storage::importDatabase(source, target, &message), Storage::ImportResult::Skipped);
    QCOMPARE(Storage::inspect(target).users, 1);

    // No old base: nothing is created.
    const QString nowhere = m_dir->filePath(QStringLiteral("other/server.db"));
    QCOMPARE(Storage::importDatabase(m_dir->filePath(QStringLiteral("missing.db")), nowhere, &message),
             Storage::ImportResult::Skipped);
    QVERIFY(!QFileInfo::exists(nowhere));

    // An empty base the service made on its own is replaced.
    const QString empty = m_dir->filePath(QStringLiteral("empty/server.db"));
    {
        Storage fresh;
        QVERIFY(fresh.open(empty));
    }
    QCOMPARE(Storage::importDatabase(source, empty, &message), Storage::ImportResult::Imported);
    QCOMPARE(Storage::inspect(empty).users, 2);
}

/*! An older server does not write its schema over a newer base. */
void ServerTest::newerBaseIsRefused()
{
    const QString path = m_dir->filePath(QStringLiteral("newer.db"));
    {
        Storage storage;
        QVERIFY(storage.open(path));
    }
    rows(path, QStringLiteral("PRAGMA user_version = 99"));

    Storage storage;
    QString error;
    QVERIFY(!storage.open(path, &error));
    QVERIFY2(error.contains(QStringLiteral("более новой")), qPrintable(error));
    QVERIFY(!Storage::inspect(path).ok);
    QCOMPARE(rows(path, QStringLiteral("PRAGMA user_version")).value(0).value(0).toInt(), 99);
}

/*!
 * A base of version 2 counted no exchanges; what it knows - the day every
 * device was last seen - becomes the start of the history.
 */
void ServerTest::olderBaseStartsTheHistoryFromSessions()
{
    const QString path = m_dir->filePath(QStringLiteral("v2.db"));
    {
        Storage storage;
        QVERIFY(storage.open(path));
        const UserInfo user = storage.createUser(QStringLiteral("galya"), QStringLiteral("Галя"),
                                                 QStringLiteral("hash"));
        QVERIFY(user.isValid());
        QVERIFY(storage.addToken(QByteArray(32, 'x'), user.id, QStringLiteral("phone")));
    }
    rows(path, QStringLiteral("DROP TABLE activity"));
    rows(path, QStringLiteral("PRAGMA user_version = 2"));

    {
        Storage storage;
        QVERIFY(storage.open(path));
    }
    const QList<QVariantList> activity = rows(path, QStringLiteral("SELECT day, syncs FROM activity"));
    QCOMPARE(activity.size(), 1);
    QCOMPARE(activity.first().at(0).toString(), QDate::currentDate().toString(Qt::ISODate));
    QCOMPARE(activity.first().at(1).toInt(), 0);
    QCOMPARE(rows(path, QStringLiteral("PRAGMA user_version")).value(0).value(0).toInt(),
             Storage::schemaVersion());
}
