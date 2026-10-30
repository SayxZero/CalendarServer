#include "api.h"
#include "config.h"
#include "httpserver.h"
#include "metrics.h"
#include "runtimefile.h"
#include "server.h"
#include "storage.h"
#include "workerpool.h"

#include <QCommandLineParser>
#include <QElapsedTimer>
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
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <climits>
#include <mutex>

/*!
 * The server is started in-process on a free port and talked to over real
 * HTTP, so the parser, the threads, the routes and the storage are tested
 * together.
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
    void sessionsAreListedWithoutSecrets();
    void revokedSessionGets401();
    void foreignSessionCannotBeRevoked();
    void throttlesGuessing();
    void forwardedForIsIgnoredFromUntrustedPeers();
    void trustedProxyCountsVisitorsApart();
    void pipelinedRequests();
    void malformedRequests();
    void headAndOldHttpAreAnsweredProperly();
    void registrationCanBeClosed();
    void clockAheadDoesNotWinForever();
    void oneServerPerBase();

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

    void requestsAreSpreadOverThreads();
    void concurrentWritesKeepEveryChange();
    void sharedCalendarHoldsUnderConcurrentEdits();
    void overloadSlowsDownInsteadOfFailing();
    void idleConnectionsMakeRoom();
    void lateRequestsAreTurnedAway();
    void bodiesOverTheBudgetWait();
    void abandonedRequestsAreNotRun();
    void slowHeadersAreCutOff();
    void everyDispatchModeAnswers_data();
    void everyDispatchModeAnswers();
    void fairLineTakesTurns();
    void poolSpreadsWorkEvenly();
    void metricsAreRecorded();
    void metricsKeepAYearAtMost();
    void resourcesAreMeasured();
    void groupCommitTakesManyOperationsAtOnce();
    void loadSettingsAreRead();

private:
    struct Reply
    {
        int status = 0;
        QJsonObject body;
    };

    Reply call(const QByteArray &method, const QString &path, const QJsonObject &body = {},
               const QString &token = QString(),
               const QHash<QByteArray, QByteArray> &headers = {});
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

    /*! Starts m_server on m_config, with the metrics ticked by hand. */
    void startServer(const Server::Tuning &tuning = Server::Tuning());

    QTemporaryDir *m_dir = nullptr;
    Storage *m_storage = nullptr;   //!< the connection of the test's own thread
    Server *m_server = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    ServerConfig m_config;
};

void ServerTest::init()
{
    m_dir = new QTemporaryDir;
    QVERIFY(m_dir->isValid());

    m_config = ServerConfig();
    m_config.dataDir = m_dir->path();
    m_config.host = QHostAddress::LocalHost;
    m_config.port = 0;
    m_config.maxBodyMb = 1;
    m_config.threads = 4;
    m_config.passwordIterations = 1000;   // the real count would make the suite slow

    startServer();
    m_network = new QNetworkAccessManager;
}

void ServerTest::startServer(const Server::Tuning &tuning)
{
    delete m_server;
    Server::Tuning tune = tuning;
    tune.metricsTickMs = 0;
    tune.flushMs = 0;
    m_server = new Server(m_config);
    m_server->setTuning(tune);
    QString error;
    QVERIFY2(m_server->start(&error), qPrintable(error));
    m_storage = &m_server->storage();
}

void ServerTest::cleanup()
{
    delete m_network;
    delete m_server;
    delete m_dir;
    m_network = nullptr;
    m_server = nullptr;
    m_storage = nullptr;
    m_dir = nullptr;
}

ServerTest::Reply ServerTest::call(const QByteArray &method, const QString &path,
                                   const QJsonObject &body, const QString &token,
                                   const QHash<QByteArray, QByteArray> &headers)
{
    QNetworkRequest request(
        QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_server->port()).arg(path)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!token.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + token.toLatin1());
    for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        request.setRawHeader(it.key(), it.value());

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
    socket.connectToHost(QHostAddress::LocalHost, m_server->port());
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
    // An unknown path is unknown whoever asks, and a known one asked the
    // wrong way is the wrong method - neither is a question of the token.
    QCOMPARE(call("GET", QStringLiteral("/api/nothing")).status, 404);
    QCOMPARE(call("GET", QStringLiteral("/elsewhere")).status, 404);
    QCOMPARE(call("GET", QStringLiteral("/api/sync")).status, 405);

    const QString token = registerUser(QStringLiteral("zoya"));
    QCOMPARE(call("POST", QStringLiteral("/api/me"), QJsonObject(), token).status, 405);

    // The scheme of the header is case-insensitive.
    const QByteArray lower = "GET /api/me HTTP/1.1\r\nAuthorization: bearer " + token.toLatin1()
                             + "\r\nConnection: close\r\n\r\n";
    QVERIFY(raw(lower).startsWith("HTTP/1.1 200"));
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

void ServerTest::sessionsAreListedWithoutSecrets()
{
    const QString first = registerUser(QStringLiteral("harry"), QStringLiteral("secret1"));
    QJsonObject credentials;
    credentials.insert(QStringLiteral("login"), QStringLiteral("harry"));
    credentials.insert(QStringLiteral("password"), QStringLiteral("secret1"));
    credentials.insert(QStringLiteral("device"), QStringLiteral("ИИ-агент · Клиент"));
    const QString second = call("POST", QStringLiteral("/api/login"), credentials)
                               .body.value(QStringLiteral("token")).toString();

    QCOMPARE(call("GET", QStringLiteral("/api/sessions")).status, 401);
    QCOMPARE(call("POST", QStringLiteral("/api/sessions"), QJsonObject(), first).status, 405);

    const Reply reply = call("GET", QStringLiteral("/api/sessions"), {}, first);
    QCOMPARE(reply.status, 200);
    const QJsonArray list = reply.body.value(QStringLiteral("sessions")).toArray();
    QCOMPARE(list.size(), 2);

    QSet<QString> devices;
    int current = 0;
    for (const QJsonValue &value : list) {
        const QJsonObject session = value.toObject();
        devices.insert(session.value(QStringLiteral("device")).toString());
        current += session.value(QStringLiteral("current")).toBool() ? 1 : 0;
        QVERIFY(QDateTime::fromString(session.value(QStringLiteral("createdAt")).toString(),
                                      Qt::ISODate).isValid());
        QVERIFY(QDateTime::fromString(session.value(QStringLiteral("lastUsedAt")).toString(),
                                      Qt::ISODate).isValid());
        // Exactly these fields: anything added here would leave the server.
        QCOMPARE(session.keys().size(), 5);
    }
    QCOMPARE(current, 1);
    QVERIFY(devices.contains(QStringLiteral("ИИ-агент · Клиент")));

    // The id is neither the key nor the hash the base keeps of it.
    const QByteArray answer = QJsonDocument(reply.body).toJson();
    for (const QString &token : { first, second }) {
        const QByteArray hash = QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256);
        QVERIFY(!answer.contains(token.toUtf8()));
        QVERIFY(!answer.contains(hash.toHex()));
        QVERIFY(!answer.contains(hash.toBase64()));
    }
}

void ServerTest::revokedSessionGets401()
{
    const QString owner = registerUser(QStringLiteral("irina"), QStringLiteral("secret1"));
    QJsonObject credentials;
    credentials.insert(QStringLiteral("login"), QStringLiteral("irina"));
    credentials.insert(QStringLiteral("password"), QStringLiteral("secret1"));
    credentials.insert(QStringLiteral("device"), QStringLiteral("ИИ-агент · Клиент"));
    const QString agent = call("POST", QStringLiteral("/api/login"), credentials)
                              .body.value(QStringLiteral("token")).toString();
    QCOMPARE(call("GET", QStringLiteral("/api/me"), {}, agent).status, 200);

    QString id;
    const QJsonArray list =
        call("GET", QStringLiteral("/api/sessions"), {}, owner).body.value(QStringLiteral("sessions")).toArray();
    for (const QJsonValue &value : list) {
        if (!value.toObject().value(QStringLiteral("current")).toBool())
            id = value.toObject().value(QStringLiteral("id")).toString();
    }
    QVERIFY(!id.isEmpty());

    QJsonObject revoke;
    QCOMPARE(call("POST", QStringLiteral("/api/sessions/revoke"), revoke, owner).status, 400);
    revoke.insert(QStringLiteral("id"), id);
    QCOMPARE(call("POST", QStringLiteral("/api/sessions/revoke"), revoke).status, 401);
    QCOMPARE(call("POST", QStringLiteral("/api/sessions/revoke"), revoke, owner).status, 200);

    // Every request of that session from now on, and only of that session.
    QCOMPARE(call("GET", QStringLiteral("/api/me"), {}, agent).status, 401);
    QJsonObject exchange;
    exchange.insert(QStringLiteral("since"), 0);
    QCOMPARE(call("POST", QStringLiteral("/api/sync"), exchange, agent).status, 401);
    QCOMPARE(call("GET", QStringLiteral("/api/me"), {}, owner).status, 200);

    // A second press finds nothing, and the list has moved on.
    QCOMPARE(call("POST", QStringLiteral("/api/sessions/revoke"), revoke, owner).status, 404);
    QCOMPARE(call("GET", QStringLiteral("/api/sessions"), {}, owner)
                 .body.value(QStringLiteral("sessions")).toArray().size(), 1);
}

void ServerTest::foreignSessionCannotBeRevoked()
{
    const QString mine = registerUser(QStringLiteral("jakob"));
    const QString theirs = registerUser(QStringLiteral("klara"));
    const QString id = call("GET", QStringLiteral("/api/sessions"), {}, theirs)
                           .body.value(QStringLiteral("sessions")).toArray().at(0).toObject()
                           .value(QStringLiteral("id")).toString();
    QVERIFY(!id.isEmpty());

    QJsonObject revoke;
    revoke.insert(QStringLiteral("id"), id);
    QCOMPARE(call("POST", QStringLiteral("/api/sessions/revoke"), revoke, mine).status, 404);
    QCOMPARE(call("GET", QStringLiteral("/api/me"), {}, theirs).status, 200);
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

    // Only for this login: everybody else behind the same address - a home
    // router, a reverse proxy - signs in as before.
    const QString token = registerUser(QStringLiteral("ilya"));
    QJsonObject other;
    other.insert(QStringLiteral("login"), QStringLiteral("ilya"));
    other.insert(QStringLiteral("password"), QStringLiteral("secret1"));
    QCOMPARE(call("POST", QStringLiteral("/api/login"), other).status, 200);

    // The current password is not guessed through the change of it either.
    QJsonObject change;
    change.insert(QStringLiteral("oldPassword"), QStringLiteral("guess00"));
    change.insert(QStringLiteral("newPassword"), QStringLiteral("secret2"));
    for (int i = 0; i < 10; ++i)
        QCOMPARE(call("POST", QStringLiteral("/api/password"), change, token).status, 403);
    change.insert(QStringLiteral("oldPassword"), QStringLiteral("secret1"));
    QCOMPARE(call("POST", QStringLiteral("/api/password"), change, token).status, 429);
}

void ServerTest::forwardedForIsIgnoredFromUntrustedPeers()
{
    const auto attempt = [this](const QString &login, const QString &password,
                                const QByteArray &forwardedFor) {
        QJsonObject body;
        body.insert(QStringLiteral("login"), login);
        body.insert(QStringLiteral("password"), password);
        return call("POST", QStringLiteral("/api/login"), body, QString(),
                    { { "X-Forwarded-For", forwardedFor } })
            .status;
    };

    // No proxy is trusted: a guesser cannot become a new visitor with every
    // try by writing another address into the header.
    registerUser(QStringLiteral("hanna"));
    for (int i = 1; i <= 10; ++i) {
        QCOMPARE(attempt(QStringLiteral("hanna"), QStringLiteral("guess00"),
                         "203.0.113." + QByteArray::number(i)),
                 401);
    }
    QCOMPARE(attempt(QStringLiteral("hanna"), QStringLiteral("secret1"), "203.0.113.99"), 429);

    // A proxy is trusted, but not the address the requests come from.
    m_config.trustedProxies = { QHostAddress::parseSubnet(QStringLiteral("10.0.0.0/8")) };
    startServer();
    registerUser(QStringLiteral("ilya"));
    for (int i = 1; i <= 10; ++i) {
        QCOMPARE(attempt(QStringLiteral("ilya"), QStringLiteral("guess00"),
                         "203.0.113." + QByteArray::number(i)),
                 401);
    }
    QCOMPARE(attempt(QStringLiteral("ilya"), QStringLiteral("secret1"), "203.0.113.99"), 429);
}

void ServerTest::trustedProxyCountsVisitorsApart()
{
    // The proxies as server.ini and the command line give them.
    const QString ini = m_dir->filePath(QStringLiteral("proxies.ini"));
    const auto writeIni = [&](const QByteArray &text) {
        QFile file(ini);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("[server]\n" + text);
    };
    writeIni("trusted_proxies=127.0.0.1, ::1, 10.0.0.0/8\n");
    ServerConfig config;
    QString error;
    QVERIFY2(config.loadIni(ini, &error), qPrintable(error));
    QCOMPARE(config.trustedProxies.size(), 3);
    QCOMPARE(config.trustedProxiesText(), QStringLiteral("127.0.0.1, ::1, 10.0.0.0/8"));
    writeIni("trusted_proxies=localhost\n");
    ServerConfig wrong;
    QVERIFY(!wrong.loadIni(ini, &error));
    QVERIFY(error.contains(QStringLiteral("trusted_proxies")));

    const ServerOptions options;
    QCommandLineParser parser;
    parser.addOptions(options.all());
    QVERIFY(parser.parse({ QStringLiteral("CalendarServer"), QStringLiteral("--trusted-proxies"),
                           QStringLiteral("127.0.0.1"), QStringLiteral("--trusted-proxies"),
                           QStringLiteral("192.168.0.0/16,::1") }));
    ServerConfig fromLine;
    QVERIFY2(fromLine.apply(parser, options, &error), qPrintable(error));
    QCOMPARE(fromLine.trustedProxiesText(), QStringLiteral("127.0.0.1, 192.168.0.0/16, ::1"));

    m_config.trustedProxies = config.trustedProxies;
    startServer();

    const auto attempt = [this](const QString &login, const QString &password,
                                const QByteArray &forwardedFor = QByteArray()) {
        QJsonObject body;
        body.insert(QStringLiteral("login"), login);
        body.insert(QStringLiteral("password"), password);
        QHash<QByteArray, QByteArray> headers;
        if (!forwardedFor.isEmpty())
            headers.insert("X-Forwarded-For", forwardedFor);
        return call("POST", QStringLiteral("/api/login"), body, QString(), headers).status;
    };
    const QByteArray anna = "203.0.113.1";
    const QByteArray boris = "203.0.113.2";

    // Anna guesses Hanna's password until the login is closed - to her.
    registerUser(QStringLiteral("hanna"));
    for (int i = 0; i < 10; ++i)
        QCOMPARE(attempt(QStringLiteral("hanna"), QStringLiteral("guess00"), anna), 401);
    QCOMPARE(attempt(QStringLiteral("hanna"), QStringLiteral("secret1"), anna), 429);
    // What she writes at the left end does not make her somebody else: the
    // proxy added her address last. Nor does another trusted hop on the way.
    QCOMPARE(attempt(QStringLiteral("hanna"), QStringLiteral("secret1"), "198.51.100.7, " + anna),
             429);
    QCOMPARE(attempt(QStringLiteral("hanna"), QStringLiteral("secret1"), anna + ", 10.0.0.5"),
             429);

    // Boris behind the same proxy signs in as before, and so does a request
    // of the proxy itself.
    QCOMPARE(attempt(QStringLiteral("hanna"), QStringLiteral("secret1"), boris), 200);
    QCOMPARE(attempt(QStringLiteral("hanna"), QStringLiteral("secret1")), 200);

    // Fifty wrong passwords over many logins close Anna's address as a whole,
    // and only hers.
    registerUser(QStringLiteral("ilya"));
    for (int i = 0; i < 40; ++i) {
        QCOMPARE(attempt(QStringLiteral("nobody%1").arg(i), QStringLiteral("guess00"), anna),
                 401);
    }
    QCOMPARE(attempt(QStringLiteral("ilya"), QStringLiteral("secret1"), anna), 429);
    QCOMPARE(attempt(QStringLiteral("ilya"), QStringLiteral("secret1"), boris), 200);
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

void ServerTest::headAndOldHttpAreAnsweredProperly()
{
    // HEAD: the headers of the GET, its length included, and no body.
    const QByteArray get = raw("GET /api/ping HTTP/1.1\r\nConnection: close\r\n\r\n");
    const QByteArray head = raw("HEAD /api/ping HTTP/1.1\r\nConnection: close\r\n\r\n");
    const auto lengthOf = [](const QByteArray &answer) {
        const int at = answer.indexOf("Content-Length: ");
        return at < 0 ? -1 : answer.mid(at + 16, answer.indexOf("\r\n", at) - at - 16).toInt();
    };
    QVERIFY(head.startsWith("HTTP/1.1 200"));
    QVERIFY(lengthOf(get) > 0);
    QCOMPARE(lengthOf(head), lengthOf(get));
    QVERIFY(head.endsWith("\r\n\r\n"));

    // HTTP/1.0 closes the connection unless asked to keep it.
    QVERIFY(raw("GET /api/ping HTTP/1.0\r\n\r\n").contains("Connection: close"));
    QVERIFY(raw("GET /api/ping HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
                .contains("Connection: keep-alive"));
}

void ServerTest::clockAheadDoesNotWinForever()
{
    const QString token = registerUser(QStringLiteral("yana"));
    QJsonObject first;
    first.insert(QStringLiteral("title"), QStringLiteral("С телефона, у которого 2036 год"));
    QJsonObject push;
    push.insert(QStringLiteral("since"), 0);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 tenYears = 10ll * 365 * 24 * 3600 * 1000;
    push.insert(QStringLiteral("changes"),
                QJsonArray { change(QStringLiteral("event"), QStringLiteral("e1"), now + tenYears,
                                    first) });
    QCOMPARE(call("POST", QStringLiteral("/api/sync"), push, token).status, 200);

    // The device that is right about the time edits the same record a minute
    // later: before, its edit lost to the one from the future for ten years.
    QJsonObject second;
    second.insert(QStringLiteral("title"), QStringLiteral("Правка с компьютера"));
    push.insert(QStringLiteral("changes"),
                QJsonArray { change(QStringLiteral("event"), QStringLiteral("e1"), now + 60000,
                                    second) });
    const Reply reply = call("POST", QStringLiteral("/api/sync"), push, token);
    QCOMPARE(reply.status, 200);
    QCOMPARE(reply.body.value(QStringLiteral("accepted")).toInt(), 1);
    QCOMPARE(reply.body.value(QStringLiteral("rejected")).toInt(), 0);

    const QHash<QString, QJsonObject> all = pull(token);
    QCOMPARE(all.value(QStringLiteral("event/e1")).value(QStringLiteral("data")).toObject()
                 .value(QStringLiteral("title")).toString(),
             QStringLiteral("Правка с компьютера"));
}

void ServerTest::oneServerPerBase()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());

    RuntimeFile first(folder.path());
    QVERIFY(first.claim());
    RuntimeInfo info;
    info.port = 8799;
    info.logFile = folder.filePath(QStringLiteral("server.log"));
    info.version = QStringLiteral("test");
    first.publish(info);

    // The admin panel learns that the base is in use, and from whom.
    const RuntimeInfo seen = RuntimeFile::read(folder.path());
    QVERIFY(seen.running);
    QCOMPARE(seen.pid, QCoreApplication::applicationPid());
    QCOMPARE(seen.port, quint16(8799));
    QCOMPARE(seen.logFile, info.logFile);

    // A second server on the same base does not start.
    RuntimeFile second(folder.path());
    QString error;
    QVERIFY(!second.claim(&error));
    QVERIFY(error.contains(QString::number(QCoreApplication::applicationPid())));

    // Once the first one is gone the base is free again.
    first.release();
    QVERIFY(!RuntimeFile::read(folder.path()).running);
    QVERIFY(second.claim());
    second.release();
}

void ServerTest::registrationCanBeClosed()
{
    const ServerConfig open = m_config;
    m_config.allowRegistration = false;
    startServer();

    QJsonObject body;
    body.insert(QStringLiteral("login"), QStringLiteral("ivan"));
    body.insert(QStringLiteral("password"), QStringLiteral("secret1"));
    QCOMPARE(call("POST", QStringLiteral("/api/register"), body).status, 403);
    QVERIFY(!call("GET", QStringLiteral("/api/ping")).body.value(QStringLiteral("registration"))
                 .toBool(true));

    m_config = open;
    startServer();
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

    // Counted in memory, written every few seconds - or now.
    m_server->flush();
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

// ------------------------------------------------------------ threads and load ----

namespace {

/*!
 * One client of raw HTTP: a connection of its own and its requests sent one
 * after another, each as soon as the answer to the previous one is in. The
 * server answers on the same thread, so the client is all signals.
 */
class LoadClient : public QObject
{
public:
    LoadClient(quint16 port, QList<QByteArray> requests, int closeAfterSendMs = -1,
               bool stay = false)
        : m_port(port)
        , m_requests(std::move(requests))
        , m_closeAfterSendMs(closeAfterSendMs)
        , m_stay(stay)
    {
        connect(&m_socket, &QTcpSocket::connected, this, [this] { sendNext(); });
        connect(&m_socket, &QTcpSocket::readyRead, this, [this] { readAnswers(); });
        connect(&m_socket, &QTcpSocket::disconnected, this, [this] { m_finished = true; });
        connect(&m_socket, &QTcpSocket::errorOccurred, this, [this] {
            if (statuses.size() < m_requests.size() && m_closeAfterSendMs < 0) {
                ++failures;
                errors.append(QStringLiteral("%1 после %2 ответов").arg(m_socket.errorString())
                                  .arg(statuses.size()));
            }
            m_finished = true;
        });
    }

    void start() { m_socket.connectToHost(QHostAddress::LocalHost, m_port); }
    bool finished() const { return m_finished || statuses.size() == m_requests.size(); }

    QList<int> statuses;
    int retryAfter = 0;   //!< answers that carried Retry-After
    int failures = 0;
    QStringList errors;

private:
    void sendNext()
    {
        if (m_next >= m_requests.size()) {
            // A polite client lets the connection go; one that stays keeps it.
            if (!m_stay)
                m_socket.disconnectFromHost();
            return;
        }
        m_socket.write(m_requests.at(m_next++));
        m_waiting = QByteArrayLiteral("answer");
        if (m_closeAfterSendMs >= 0) {
            QTimer::singleShot(m_closeAfterSendMs, this, [this] {
                m_socket.abort();
                m_finished = true;
            });
        }
    }

    void readAnswers()
    {
        m_buffer += m_socket.readAll();
        for (;;) {
            const int headEnd = m_buffer.indexOf("\r\n\r\n");
            if (headEnd < 0)
                return;
            const QByteArray head = m_buffer.left(headEnd);
            qint64 length = 0;
            for (const QByteArray &line : head.split('\n')) {
                const QByteArray lower = line.trimmed().toLower();
                if (lower.startsWith("content-length:"))
                    length = lower.mid(15).trimmed().toLongLong();
                if (lower.startsWith("retry-after:"))
                    ++retryAfter;
            }
            if (m_buffer.size() < headEnd + 4 + length)
                return;
            statuses.append(head.mid(9, 3).toInt());
            m_buffer.remove(0, int(headEnd + 4 + length));
            m_waiting.clear();
            sendNext();
        }
    }

    QTcpSocket m_socket;
    quint16 m_port;
    QList<QByteArray> m_requests;
    int m_closeAfterSendMs;
    bool m_stay;
    int m_next = 0;
    QByteArray m_buffer;
    QByteArray m_waiting;
    bool m_finished = false;
};

struct LoadResult
{
    QList<int> statuses;
    int retryAfter = 0;
    int failures = 0;
    QStringList errors;
    qint64 elapsedMs = 0;

    int count(int status) const { return int(std::count(statuses.begin(), statuses.end(), status)); }
};

/*! Runs one LoadClient a list, all at once, until every one is done. */
LoadResult runLoad(quint16 port, const QList<QList<QByteArray>> &connections, int timeoutMs = 60000)
{
    QList<LoadClient *> clients;
    for (const QList<QByteArray> &requests : connections)
        clients.append(new LoadClient(port, requests));
    QElapsedTimer timer;
    timer.start();
    for (LoadClient *client : clients)
        client->start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (std::all_of(clients.begin(), clients.end(), [](LoadClient *c) { return c->finished(); }))
            break;
    }
    LoadResult result;
    result.elapsedMs = timer.elapsed();
    for (LoadClient *client : clients) {
        result.statuses += client->statuses;
        result.retryAfter += client->retryAfter;
        result.failures += client->failures;
        result.errors += client->errors;
        delete client;
    }
    return result;
}

QByteArray httpRequest(const QByteArray &method, const QString &path, const QJsonObject &body,
                       const QString &token = QString())
{
    const QByteArray payload = method == "GET" ? QByteArray()
                                               : QJsonDocument(body).toJson(QJsonDocument::Compact);
    QByteArray request = method + ' ' + path.toUtf8() + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!token.isEmpty())
        request += "Authorization: Bearer " + token.toLatin1() + "\r\n";
    request += "Content-Type: application/json\r\nContent-Length: "
               + QByteArray::number(payload.size()) + "\r\n\r\n" + payload;
    return request;
}

QByteArray loginRequest(const QString &login, const QString &password = QStringLiteral("secret1"))
{
    QJsonObject body;
    body.insert(QStringLiteral("login"), login);
    body.insert(QStringLiteral("password"), password);
    return httpRequest("POST", QStringLiteral("/api/login"), body);
}

QByteArray pushRequest(const QString &token, const QString &id, const QString &calendar,
                       const QString &title, qint64 modified, int padding = 0)
{
    QJsonObject data;
    data.insert(QStringLiteral("id"), id);
    data.insert(QStringLiteral("title"), title);
    if (!calendar.isEmpty())
        data.insert(QStringLiteral("calendarId"), calendar);
    if (padding > 0)
        data.insert(QStringLiteral("notes"), QString(padding, QLatin1Char('x')));
    QJsonObject change;
    change.insert(QStringLiteral("kind"), QStringLiteral("event"));
    change.insert(QStringLiteral("id"), id);
    change.insert(QStringLiteral("modified"), double(modified));
    change.insert(QStringLiteral("data"), data);
    QJsonObject body;
    body.insert(QStringLiteral("since"), 0);
    body.insert(QStringLiteral("device"), QStringLiteral("load"));
    body.insert(QStringLiteral("changes"), QJsonArray { change });
    return httpRequest("POST", QStringLiteral("/api/sync"), body, token);
}

} // namespace

/*!
 * Requests are answered on several threads at once, and every thread gets
 * its share: the slow part of a login (the password hash) runs in parallel.
 */
void ServerTest::requestsAreSpreadOverThreads()
{
    m_config.passwordIterations = 40000;
    startServer();
    QVERIFY(!registerUser(QStringLiteral("anna")).isEmpty());

    const PoolSnapshot before = m_server->pool().snapshot();
    QList<QList<QByteArray>> connections;
    for (int c = 0; c < 16; ++c)
        connections.append(QList<QByteArray>(8, loginRequest(QStringLiteral("anna"))));
    const LoadResult result = runLoad(m_server->port(), connections);
    QCOMPARE(result.failures, 0);
    QCOMPARE(result.count(200), 16 * 8);

    const PoolSnapshot after = m_server->pool().snapshot();
    QCOMPARE(after.threads, 4);
    qint64 total = 0;
    qint64 busyNs = 0;
    qint64 least = LLONG_MAX;
    qint64 most = 0;
    for (int i = 0; i < after.threads; ++i) {
        const qint64 done = after.doneByThread.at(i) - before.doneByThread.at(i);
        total += done;
        least = qMin(least, done);
        most = qMax(most, done);
        busyNs += after.busyNsByThread.at(i) - before.busyNsByThread.at(i);
    }
    QCOMPARE(total, qint64(16 * 8));
    // Evenly: the busiest thread did no more than twice what the idlest did.
    QVERIFY2(least > 0 && most <= 2 * least,
             qPrintable(QStringLiteral("от %1 до %2 запросов на поток").arg(least).arg(most)));
    // And at once: the threads worked longer in total than the wall clock ran.
    const double parallel = double(busyNs) / 1e6 / double(qMax<qint64>(1, result.elapsedMs));
    QVERIFY2(parallel > 1.5, qPrintable(QStringLiteral("параллельность %1").arg(parallel)));
}

/*!
 * Many devices of one account writing at the same moment: every change is
 * kept, and every one gets a revision of its own - a revision handed out
 * twice would make a device skip a change for good.
 */
void ServerTest::concurrentWritesKeepEveryChange()
{
    const QString token = registerUser(QStringLiteral("boris"));
    QList<QList<QByteArray>> connections;
    for (int device = 0; device < 8; ++device) {
        QList<QByteArray> requests;
        for (int i = 0; i < 25; ++i) {
            const QString id = QStringLiteral("d%1-e%2").arg(device).arg(i);
            requests.append(pushRequest(token, id, QString(), id, 1000 + i));
        }
        connections.append(requests);
    }
    const LoadResult result = runLoad(m_server->port(), connections);
    QCOMPARE(result.failures, 0);
    QCOMPARE(result.count(200), 200);

    const QHash<QString, QJsonObject> all = pull(token);
    QCOMPARE(all.size(), 200);
    QSet<qint64> revisions;
    for (const QJsonObject &record : all)
        revisions.insert(qint64(record.value(QStringLiteral("revision")).toDouble()));
    QCOMPARE(revisions.size(), 200);
    QCOMPARE(m_cursor, qint64(200));
    QCOMPARE(*std::max_element(revisions.begin(), revisions.end()), qint64(200));
    // Every write went through the gate, one at a time.
    QVERIFY(m_storage->takeGateStats().writes >= 200);
}

/*!
 * The owner and two members writing into one shared calendar at once: in
 * the end all three hold the same records.
 */
void ServerTest::sharedCalendarHoldsUnderConcurrentEdits()
{
    const QString owner = registerUser(QStringLiteral("olga"));
    const QString first = registerUser(QStringLiteral("petr"));
    const QString second = registerUser(QStringLiteral("rita"));
    QCOMPARE(put(owner, QStringLiteral("calendar"), QStringLiteral("home"), QString(),
                 QStringLiteral("Дом"), 1000).status,
             200);
    QJsonObject configure;
    configure.insert(QStringLiteral("calendar"), QStringLiteral("home"));
    configure.insert(QStringLiteral("mode"), QStringLiteral("edit"));
    QCOMPARE(call("POST", QStringLiteral("/api/share"), configure, owner).status, 200);
    const QString link = shareLink(owner, QStringLiteral("home"), false);
    for (const auto &member : { qMakePair(first, QStringLiteral("twin-p")),
                                qMakePair(second, QStringLiteral("twin-r")) }) {
        QJsonObject join;
        join.insert(QStringLiteral("token"), link);
        join.insert(QStringLiteral("twinId"), member.second);
        QCOMPARE(call("POST", QStringLiteral("/api/share/join"), join, member.first).status, 201);
    }

    const QList<QPair<QString, QString>> writers = { { owner, QStringLiteral("home") },
                                                     { first, QStringLiteral("twin-p") },
                                                     { second, QStringLiteral("twin-r") } };
    QList<QList<QByteArray>> connections;
    for (int w = 0; w < writers.size(); ++w) {
        QList<QByteArray> requests;
        for (int i = 0; i < 20; ++i) {
            const QString id = QStringLiteral("w%1-%2").arg(w).arg(i);
            requests.append(pushRequest(writers.at(w).first, id, writers.at(w).second, id, 2000 + i));
        }
        connections.append(requests);
    }
    const LoadResult result = runLoad(m_server->port(), connections);
    QCOMPARE(result.failures, 0);
    QCOMPARE(result.count(200), 60);

    for (const auto &writer : writers) {
        int events = 0;
        const QHash<QString, QJsonObject> records = pull(writer.first);
        for (const QJsonObject &record : records) {
            if (record.value(QStringLiteral("kind")).toString() != QLatin1String("event"))
                continue;
            ++events;
            QCOMPARE(record.value(QStringLiteral("data")).toObject()
                         .value(QStringLiteral("calendarId")).toString(),
                     writer.second);
        }
        QCOMPARE(events, 60);
    }
}

/*!
 * Far more clients than the server takes at once: it slows down - the rest
 * wait in the queue of the system - but every one of them gets the answer,
 * and the server answers as usual afterwards.
 */
void ServerTest::overloadSlowsDownInsteadOfFailing()
{
    m_config.passwordIterations = 20000;
    Server::Tuning tuning;
    tuning.customLimits = true;
    tuning.limits.maxBody = 1024 * 1024;
    tuning.limits.maxConnections = 8;
    m_config.threads = 2;
    startServer(tuning);
    QVERIFY(!registerUser(QStringLiteral("vera")).isEmpty());

    QList<QList<QByteArray>> connections;
    for (int c = 0; c < 64; ++c)
        connections.append(QList<QByteArray>(3, loginRequest(QStringLiteral("vera"))));
    const LoadResult result = runLoad(m_server->port(), connections, 120000);
    QVERIFY2(result.failures == 0, qPrintable(result.errors.join(QStringLiteral("; "))));
    QCOMPARE(result.count(200), 64 * 3);

    const HttpSnapshot http = m_server->http().snapshot(true);
    QVERIFY2(http.maxConnections <= 8, qPrintable(QString::number(http.maxConnections)));
    QVERIFY(!http.paused);
    QCOMPARE(call("GET", QStringLiteral("/api/ping")).status, 200);
}

/*!
 * At the connection limit, kept-alive connections that idle are closed to
 * let a waiting client in: without it, it would wait for their timeout.
 */
void ServerTest::idleConnectionsMakeRoom()
{
    Server::Tuning tuning;
    tuning.customLimits = true;
    tuning.limits.maxBody = 1024 * 1024;
    tuning.limits.maxConnections = 2;
    startServer(tuning);

    const QByteArray ping = httpRequest("GET", QStringLiteral("/api/ping"), {});
    LoadClient first(m_server->port(), { ping }, -1, true);
    LoadClient second(m_server->port(), { ping }, -1, true);
    first.start();
    second.start();
    QTRY_VERIFY(first.statuses.size() == 1 && second.statuses.size() == 1);

    QElapsedTimer timer;
    timer.start();
    LoadClient third(m_server->port(), { ping });
    third.start();
    QTRY_VERIFY_WITH_TIMEOUT(third.statuses.size() == 1, 10000);
    QCOMPARE(third.statuses.first(), 200);
    // Not the idle timeout of a minute: about a second of idling.
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
}

/*!
 * A request that stood in line longer than a client waits is answered 503
 * with Retry-After instead of being run: the work would be for nothing.
 */
void ServerTest::lateRequestsAreTurnedAway()
{
    m_config.passwordIterations = 60000;
    m_config.threads = 1;
    Server::Tuning tuning;
    tuning.customLimits = true;
    tuning.limits.maxBody = 1024 * 1024;
    tuning.limits.maxQueueWaitMs = 20;
    tuning.limits.retryAfterSeconds = 7;
    startServer(tuning);
    QVERIFY(!registerUser(QStringLiteral("gleb")).isEmpty());

    LoadClient first(m_server->port(), { loginRequest(QStringLiteral("gleb")) });
    first.start();
    QTRY_COMPARE(m_server->pool().busy(), 1);
    QList<QList<QByteArray>> connections;
    for (int c = 0; c < 5; ++c)
        connections.append({ loginRequest(QStringLiteral("gleb")) });
    const LoadResult result = runLoad(m_server->port(), connections);
    QTRY_VERIFY(first.finished());
    QCOMPARE(first.statuses, QList<int>({ 200 }));
    QCOMPARE(result.failures, 0);
    QCOMPARE(result.statuses.size(), 5);
    QVERIFY(result.count(503) >= 1);
    QCOMPARE(result.retryAfter, result.count(503));
    QCOMPARE(m_server->http().snapshot().overloaded, qint64(result.count(503)));

    // The line of the pool itself has a limit too.
    WorkerPool pool(1, DispatchMode::Fair, 2);
    QVERIFY(pool.start());
    std::atomic<bool> release { false };
    PoolTask blocker;
    blocker.run = [&release](int, qint64) {
        while (!release.load())
            QThread::msleep(1);
    };
    QVERIFY(pool.submit(blocker));
    QTRY_COMPARE(pool.busy(), 1);
    PoolTask idle;
    idle.run = [](int, qint64) {};
    QVERIFY(pool.submit(idle));
    QVERIFY(pool.submit(idle));
    QVERIFY(!pool.submit(idle));
    QCOMPARE(pool.snapshot().refused, qint64(1));
    release = true;
}

/*!
 * Bodies above the memory budget wait for the ones before them instead of
 * piling up in memory - and are answered all the same.
 */
void ServerTest::bodiesOverTheBudgetWait()
{
    Server::Tuning tuning;
    tuning.customLimits = true;
    tuning.limits.maxBody = 1024 * 1024;
    tuning.limits.bufferBudget = 1024 * 1024;
    startServer(tuning);
    const QString token = registerUser(QStringLiteral("dana"));

    qint64 peak = 0;
    QTimer watch;
    connect(&watch, &QTimer::timeout, this,
            [&] { peak = qMax(peak, m_server->http().snapshot().buffered); });
    watch.start(1);

    QList<QList<QByteArray>> connections;
    for (int c = 0; c < 8; ++c) {
        connections.append({ pushRequest(token, QStringLiteral("big-%1").arg(c), QString(),
                                         QStringLiteral("Большая"), 1000, 600 * 1024) });
    }
    const LoadResult result = runLoad(m_server->port(), connections);
    watch.stop();
    QCOMPARE(result.failures, 0);
    QCOMPARE(result.count(200), 8);
    // A body counts eight times its size while it is answered - more than the
    // whole budget of 1 MB: it is let in alone, one 600 KB body at a time.
    const qint64 budget = m_server->http().snapshot().bufferBudget;
    QCOMPARE(budget, qint64(1024 * 1024));
    QVERIFY2(peak > budget, qPrintable(QString::number(peak)));
    QVERIFY2(peak < 2 * 600 * 1024 * 8, qPrintable(QString::number(peak)));
    QCOMPARE(m_server->http().snapshot().buffered, qint64(0));
    QCOMPARE(pull(token).size(), 8);
}

/*!
 * A client that gave up while its request was waiting: the request is not
 * run, and its place in memory is given back.
 */
void ServerTest::abandonedRequestsAreNotRun()
{
    m_config.passwordIterations = 200000;
    m_config.threads = 1;
    startServer();
    QVERIFY(!registerUser(QStringLiteral("egor")).isEmpty());

    // The only thread is busy with a slow login; meanwhile another client
    // sends its request and goes away.
    LoadClient slow(m_server->port(), { loginRequest(QStringLiteral("egor")) });
    LoadClient quitter(m_server->port(), { loginRequest(QStringLiteral("egor")) }, 30);
    slow.start();
    QTRY_COMPARE(m_server->pool().busy(), 1);
    quitter.start();
    QTRY_VERIFY_WITH_TIMEOUT(slow.finished() && quitter.finished(), 20000);
    QTRY_COMPARE(m_server->pool().snapshot().cancelled, qint64(1));
    QCOMPARE(slow.statuses, QList<int>({ 200 }));
    QVERIFY(quitter.statuses.isEmpty());
    QTRY_COMPARE(m_server->http().snapshot().buffered, qint64(0));
}

/*!
 * Headers that trickle in byte by byte do not hold a connection for ever.
 */
void ServerTest::slowHeadersAreCutOff()
{
    Server::Tuning tuning;
    tuning.customLimits = true;
    tuning.limits.maxBody = 1024 * 1024;
    tuning.limits.headerTimeoutMs = 300;
    startServer(tuning);

    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, m_server->port());
    QVERIFY(socket.waitForConnected(5000));
    socket.write("GET /api/ping HTTP/1.1\r\nHost: 127.0.0.1\r\n");
    QTest::qWait(450);
    socket.write("X");
    QByteArray answer;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000 && socket.state() == QAbstractSocket::ConnectedState) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        answer += socket.readAll();
    }
    answer += socket.readAll();
    QVERIFY2(answer.startsWith("HTTP/1.1 408"), answer.constData());
}

/*!
 * Every way of lining requests up answers them all, whatever the mix.
 */
void ServerTest::everyDispatchModeAnswers_data()
{
    QTest::addColumn<QString>("mode");
    for (const char *mode : { "fair", "fifo", "round_robin", "least_loaded" })
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void ServerTest::everyDispatchModeAnswers()
{
    QFETCH(QString, mode);
    m_config.dispatch = mode;
    startServer();
    QCOMPARE(m_server->pool().mode(), dispatchModeFromKey(mode));
    const QString token = registerUser(QStringLiteral("zoya"));

    QList<QList<QByteArray>> connections;
    for (int c = 0; c < 12; ++c) {
        QList<QByteArray> requests;
        for (int i = 0; i < 6; ++i) {
            const QString id = QStringLiteral("%1-%2").arg(c).arg(i);
            if (i % 3 == 0)
                requests.append(loginRequest(QStringLiteral("zoya")));
            else if (i % 3 == 1)
                requests.append(pushRequest(token, id, QString(), id, 1000));
            else
                requests.append(httpRequest("GET", QStringLiteral("/api/me"), {}, token));
        }
        connections.append(requests);
    }
    const LoadResult result = runLoad(m_server->port(), connections);
    QCOMPARE(result.failures, 0);
    QCOMPARE(result.count(200), 12 * 6);
    QCOMPARE(pull(token).size(), 12 * 2);
}

/*!
 * The fair line takes turns between clients: one with a single request
 * does not wait behind the twenty of another. The plain line makes it wait.
 */
void ServerTest::fairLineTakesTurns()
{
    for (const DispatchMode mode : { DispatchMode::Fair, DispatchMode::Fifo }) {
        WorkerPool pool(1, mode, 100);
        QVERIFY(pool.start());
        std::atomic<bool> release { false };
        PoolTask blocker;
        blocker.client = QStringLiteral("a");
        blocker.run = [&release](int, qint64) {
            while (!release.load())
                QThread::msleep(1);
        };
        QVERIFY(pool.submit(blocker));
        QTRY_COMPARE(pool.busy(), 1);

        std::mutex lock;
        QStringList order;
        const auto task = [&](const QString &client) {
            PoolTask one;
            one.client = client;
            one.run = [&, client](int, qint64) {
                std::lock_guard<std::mutex> guard(lock);
                order.append(client);
            };
            return one;
        };
        for (int i = 0; i < 20; ++i)
            QVERIFY(pool.submit(task(QStringLiteral("a"))));
        QVERIFY(pool.submit(task(QStringLiteral("b"))));
        release = true;
        QTRY_COMPARE(pool.snapshot().finished, qint64(22));
        if (mode == DispatchMode::Fair)
            QVERIFY2(order.indexOf(QStringLiteral("b")) <= 1, qPrintable(order.join(',')));
        else
            QCOMPARE(order.indexOf(QStringLiteral("b")), 20);
    }
}

/*!
 * Tasks of uneven length spread over the threads: the shared line keeps
 * every thread busy, so each one ends up with about its share.
 */
void ServerTest::poolSpreadsWorkEvenly()
{
    for (const DispatchMode mode : { DispatchMode::Fair, DispatchMode::Fifo, DispatchMode::RoundRobin,
                                     DispatchMode::LeastLoaded }) {
        WorkerPool pool(4, mode, 1000);
        QVERIFY(pool.start());
        QRandomGenerator random(42);
        for (int i = 0; i < 400; ++i) {
            PoolTask task;
            task.client = QString::number(i % 10);
            const int sleep = int(random.bounded(3));
            task.run = [sleep](int, qint64) { QThread::msleep(sleep); };
            QVERIFY(pool.submit(task));
        }
        QTRY_COMPARE_WITH_TIMEOUT(pool.snapshot().finished, qint64(400), 30000);
        const PoolSnapshot shot = pool.snapshot();
        for (qint64 done : shot.doneByThread)
            QVERIFY2(done >= 50 && done <= 150, qPrintable(QStringLiteral("%1: %2")
                                                               .arg(dispatchModeKey(mode))
                                                               .arg(done)));
        QCOMPARE(pool.queued(), 0);
    }

    // Stopping cancels what still waits.
    WorkerPool pool(1, DispatchMode::Fair, 100);
    QVERIFY(pool.start());
    std::atomic<bool> release { false };
    std::atomic<int> cancelled { 0 };
    PoolTask blocker;
    blocker.run = [&release](int, qint64) {
        while (!release.load())
            QThread::msleep(1);
    };
    QVERIFY(pool.submit(blocker));
    QTRY_COMPARE(pool.busy(), 1);
    for (int i = 0; i < 5; ++i) {
        PoolTask waiting;
        waiting.run = [](int, qint64) {};
        waiting.cancel = [&cancelled] { ++cancelled; };
        QVERIFY(pool.submit(waiting));
    }
    release = true;
    pool.stop();
    QVERIFY(cancelled.load() + (pool.snapshot().finished - 1) == 5);
}

/*!
 * What the server measures lands in metrics.db: the minute, the hour it
 * adds up into, and the live values the panel shows.
 */
void ServerTest::metricsAreRecorded()
{
    const QString token = registerUser(QStringLiteral("ivan"));
    MetricsRecorder *recorder = m_server->metrics();
    QVERIFY(recorder);
    const qint64 minute = QDateTime::currentSecsSinceEpoch() / 60 * 60;
    recorder->tick(minute + 5);
    for (int i = 0; i < 10; ++i)
        QCOMPARE(call("GET", QStringLiteral("/api/me"), {}, token).status, 200);
    QCOMPARE(call("GET", QStringLiteral("/api/nothing")).status, 404);
    recorder->tick(minute + 10);
    recorder->flushMinute();

    MetricsStore store;
    QString error;
    QVERIFY2(store.open(MetricsStore::pathIn(m_dir->path()), false, &error), qPrintable(error));
    const QList<MetricsRow> minutes = store.series(minute - 60, minute + 60, 60);
    QCOMPARE(minutes.size(), 1);
    QCOMPARE(minutes.first().t, minute);
    QCOMPARE(minutes.first().requests, qint64(11));
    QCOMPARE(minutes.first().clientErrors, qint64(1));
    QCOMPARE(minutes.first().threads, 4);
    QVERIFY(minutes.first().workCount >= 10);
    QVERIFY(minutes.first().memoryProcess > 0);

    const QList<MetricsRow> hours = store.series(minute - 3600 * 24, minute + 3600, 3600);
    QCOMPARE(hours.size(), 1);
    QCOMPARE(hours.first().requests, qint64(11));

    const QJsonObject live = store.live();
    QCOMPARE(live.value(QStringLiteral("threads")).toInt(), 4);
    QCOMPARE(live.value(QStringLiteral("mode")).toString(), QStringLiteral("fair"));
    QCOMPARE(live.value(QStringLiteral("doneByThread")).toArray().size(), 4);
    QCOMPARE(qint64(live.value(QStringLiteral("at")).toDouble()), minute + 10);
    QCOMPARE(live.value(QStringLiteral("version")).toString(), QStringLiteral(SERVER_VERSION));
    QVERIFY(live.value(QStringLiteral("memoryProcess")).toDouble() > 0);
}

/*!
 * The history keeps minutes for a week and hours for a year, never longer,
 * and an hour adds its minutes up: counts summed, averages weighted, an
 * unknown processor left out.
 */
void ServerTest::metricsKeepAYearAtMost()
{
    const QString path = m_dir->filePath(QStringLiteral("history.db"));
    MetricsStore store;
    QString error;
    QVERIFY2(store.open(path, true, &error), qPrintable(error));

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 hour = (now - 3 * 3600) / 3600 * 3600;
    MetricsRow first;
    first.t = hour;
    first.requests = 10;
    first.waitCount = 10;
    first.waitSumMs = 100;
    first.waitMaxMs = 30;
    first.queueAvg = 1;
    first.queueMax = 4;
    first.cpuProcess = 10;
    first.threads = 4;
    MetricsRow second = first;
    second.t = hour + 60;
    second.requests = 30;
    second.waitCount = 30;
    second.waitSumMs = 300;
    second.waitMaxMs = 50;
    second.queueAvg = 3;
    second.queueMax = 2;
    second.cpuProcess = -1;
    QVERIFY(store.writeRow(first));
    QVERIFY(store.writeRow(second));
    QVERIFY(store.rollUpHour(hour + 1800));
    const QList<MetricsRow> rolled = store.series(hour - 86400 * 8, hour + 3600, 3600);
    QCOMPARE(rolled.size(), 1);
    QCOMPARE(rolled.first().minutes, 2);
    QCOMPARE(rolled.first().requests, qint64(40));
    QCOMPARE(rolled.first().waitAvgMs(), 10.0);
    QCOMPARE(rolled.first().waitMaxMs, 50.0);
    QCOMPARE(rolled.first().queueAvg, 2.0);
    QCOMPARE(rolled.first().queueMax, 4);
    QCOMPARE(rolled.first().cpuProcess, 10.0);

    // Old rows of both kinds, just over and just under their limits.
    const auto row = [](int span, qint64 t) {
        MetricsRow one;
        one.span = span;
        one.t = t;
        one.requests = 1;
        return one;
    };
    QVERIFY(store.writeRow(row(60, now - 8 * 86400)));
    QVERIFY(store.writeRow(row(60, now - 6 * 86400)));
    QVERIFY(store.writeRow(row(3600, (now - 400 * 86400) / 3600 * 3600)));
    QVERIFY(store.writeRow(row(3600, (now - 300 * 86400) / 3600 * 3600)));
    QVERIFY(store.prune(now));
    const QList<QVariantList> left = rows(path, QStringLiteral("SELECT span, t FROM samples ORDER BY t"));
    QCOMPARE(left.size(), 5);   // the two of the hour, its roll-up, a week's minute, a year's hour
    QVERIFY(store.firstTime() >= now - 365 * 86400);
    for (const QVariantList &kept : left) {
        const qint64 age = now - kept.at(1).toLongLong();
        QVERIFY(kept.at(0).toInt() == 60 ? age <= 7 * 86400 : age <= 365 * 86400);
    }
}

/*! The processor and the memory can be measured on this platform. */
void ServerTest::resourcesAreMeasured()
{
    ResourceProbe probe;
    const ResourceProbe::Sample firstSample = probe.sample();
    QVERIFY(firstSample.memoryProcess > 0);
    QVERIFY(firstSample.memoryTotal > firstSample.memoryProcess);
    QVERIFY(firstSample.memoryLoad > 0 && firstSample.memoryLoad <= 100);

    // Some work for the processor to show.
    QElapsedTimer timer;
    timer.start();
    volatile double sink = 0;
    while (timer.elapsed() < 200)
        sink = sink + 1.0;
    const ResourceProbe::Sample second = probe.sample();
    QVERIFY(second.cpuProcess > 0 && second.cpuProcess <= 100);
    QVERIFY(second.cpuSystem >= second.cpuProcess * 0.5 && second.cpuSystem <= 100);
}

/*!
 * Group commit: operations that come while a commit is going on wait for the
 * next one and go to the disk together - and one that fails takes back only
 * its own writes.
 */
void ServerTest::groupCommitTakesManyOperationsAtOnce()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("group.db"));
    Storage storage;
    QVERIFY(storage.open(path));
    QVERIFY(storage.enableGroupCommit(64));
    const GateStats before = storage.takeGateStats();

    // The first operation holds the commit thread until it is let go.
    std::atomic<bool> started { false };
    std::atomic<bool> release { false };
    QThread *first = QThread::create([&] {
        Storage own;
        own.open(path, nullptr, Storage::OpenMode::Attach);
        own.transact([&](Storage &s) {
            started = true;
            while (!release.load())
                QThread::msleep(1);
            return s.createUser(QStringLiteral("first"), QString(), QStringLiteral("h")).isValid();
        });
    });
    first->start();
    QTRY_VERIFY(started.load());

    // Nine more line up meanwhile, one of them failing after its write.
    std::vector<QThread *> others;
    for (int i = 0; i < 8; ++i) {
        others.push_back(QThread::create([&, i] {
            Storage own;
            own.open(path, nullptr, Storage::OpenMode::Attach);
            own.createUser(QStringLiteral("user%1").arg(i), QString(), QStringLiteral("h"));
        }));
    }
    others.push_back(QThread::create([&] {
        Storage own;
        own.open(path, nullptr, Storage::OpenMode::Attach);
        own.transact([](Storage &s) {
            s.createUser(QStringLiteral("broken"), QString(), QStringLiteral("h"));
            return false;
        });
    }));
    for (QThread *thread : others)
        thread->start();
    QTest::qWait(500);
    release = true;
    for (QThread *thread : others) {
        QVERIFY(thread->wait(10000));
        delete thread;
    }
    QVERIFY(first->wait(10000));
    delete first;

    const GateStats after = storage.takeGateStats();
    QCOMPARE(after.writes - before.writes, qint64(10));
    // The first one alone, then the nine that waited in one transaction.
    QCOMPARE(after.commits - before.commits, qint64(2));
    QVERIFY(storage.userByLogin(QStringLiteral("first")).isValid());
    for (int i = 0; i < 8; ++i)
        QVERIFY(storage.userByLogin(QStringLiteral("user%1").arg(i)).isValid());
    QVERIFY(!storage.userByLogin(QStringLiteral("broken")).isValid());
    storage.close();
}

/*!
 * The load settings come from the INI file, wrong values are refused, and a
 * server started with them runs with them - group commit off and the
 * relaxed sync on every connection included.
 */
void ServerTest::loadSettingsAreRead()
{
    const QString ini = m_dir->filePath(QStringLiteral("load.ini"));
    const auto writeIni = [&](const QByteArray &text) {
        QFile file(ini);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("[server]\n" + text);
    };
    writeIni("threads=6\ndispatch=fifo\ngroup_commit=0\nsync_mode=normal\nmax_connections=200\n"
             "buffer_mb=64\nmax_queue_wait_seconds=10\n");
    ServerConfig config;
    QString error;
    QVERIFY2(config.loadIni(ini, &error), qPrintable(error));
    QCOMPARE(config.threads, 6);
    QCOMPARE(config.dispatch, QStringLiteral("fifo"));
    QCOMPARE(config.groupCommit, 0);
    QCOMPARE(config.syncMode, QStringLiteral("normal"));
    QCOMPARE(config.maxConnections, 200);
    QCOMPARE(config.bufferMb, 64);
    QCOMPARE(config.maxQueueWaitSeconds, 10);
    for (const QString &key : ServerConfig::loadKeys())
        QVERIFY2(!config.loadValue(key).isEmpty(), qPrintable(key));
    QCOMPARE(config.loadValue(QStringLiteral("group_commit")), QStringLiteral("0"));

    writeIni("sync_mode=sometimes\n");
    ServerConfig wrong;
    QVERIFY(!wrong.loadIni(ini, &error));
    QVERIFY(error.contains(QStringLiteral("sync_mode")));
    writeIni("dispatch=random\n");
    QVERIFY(!wrong.loadIni(ini, &error));

    m_config.threads = 6;
    m_config.groupCommit = 0;
    m_config.syncMode = QStringLiteral("normal");
    startServer();
    QCOMPARE(m_server->pool().threadCount(), 6);
    QCOMPARE(m_storage->synchronousMode(), 1);
    Storage other;
    QVERIFY(other.open(m_config.databasePath(), nullptr, Storage::OpenMode::Attach));
    QCOMPARE(other.synchronousMode(), 1);
    other.close();
    const QString token = registerUser(QStringLiteral("kira"));
    QCOMPARE(put(token, QStringLiteral("event"), QStringLiteral("e1"), QString(),
                 QStringLiteral("Без пакетов"), 1000).status,
             200);
    QCOMPARE(pull(token).size(), 1);
}
