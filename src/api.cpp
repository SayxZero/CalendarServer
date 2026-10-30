#include "api.h"

#include "storage.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPasswordDigestor>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <algorithm>

namespace {

/*! Wrong passwords for one login from one address, and for one account's own password. */
const int kMaxFailures = 10;
/*! Wrong passwords from one address whatever the login: guessing through many accounts. */
const int kMaxAddressFailures = 50;
const qint64 kFailureWindowMs = 10 * 60 * 1000;

/*!
 * A moment of a change further ahead of the server's clock than this is a
 * device whose clock is wrong. Taken as it is, it would win against every
 * later edit of the record on every other device for as long as the clock
 * was ahead - years, for a phone that thinks it is 2036.
 */
const qint64 kFutureToleranceMs = 10 * 60 * 1000;

/*! The methods each route answers; anything else under /api/ is 404. */
const QHash<QString, QList<QByteArray>> &routes()
{
    static const QHash<QString, QList<QByteArray>> table = {
        { QStringLiteral("/"), { "GET", "HEAD" } },
        { QStringLiteral("/api/ping"), { "GET", "HEAD" } },
        { QStringLiteral("/api/register"), { "POST" } },
        { QStringLiteral("/api/login"), { "POST" } },
        { QStringLiteral("/api/logout"), { "POST" } },
        { QStringLiteral("/api/me"), { "GET" } },
        { QStringLiteral("/api/profile"), { "POST" } },
        { QStringLiteral("/api/password"), { "POST" } },
        { QStringLiteral("/api/sessions"), { "GET" } },
        { QStringLiteral("/api/sessions/revoke"), { "POST" } },
        { QStringLiteral("/api/sync"), { "POST" } },
        { QStringLiteral("/api/record"), { "GET" } },
        { QStringLiteral("/api/share"), { "GET", "POST" } },
        { QStringLiteral("/api/share/member"), { "POST" } },
        { QStringLiteral("/api/share/link"), { "POST" } },
        { QStringLiteral("/api/share/peek"), { "GET" } },
        { QStringLiteral("/api/share/join"), { "POST" } },
        { QStringLiteral("/api/share/leave"), { "POST" } },
    };
    return table;
}

QByteArray randomBytes(int count)
{
    QByteArray bytes(count, Qt::Uninitialized);
    QRandomGenerator *generator = QRandomGenerator::system();
    for (int i = 0; i < count; ++i)
        bytes[i] = char(generator->bounded(256));
    return bytes;
}

QByteArray tokenHash(const QString &token)
{
    return QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256);
}

/*!
 * What a session is called outside the server. The stored hash itself must not
 * leave: this one is a hash of it and answers no question about the key.
 */
QString sessionId(const QByteArray &hash)
{
    return QString::fromLatin1(
        QCryptographicHash::hash("session:" + hash, QCryptographicHash::Sha256).left(16).toHex());
}

/*! Compares without leaking through timing how many leading bytes match. */
bool sameBytes(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
    return diff == 0;
}

bool validKind(const QString &kind)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-z_]{1,32}$"));
    return pattern.match(kind).hasMatch();
}

} // namespace

Api::Api(Storage *storage, const ServerConfig &config, std::shared_ptr<LoginThrottle> throttle)
    : m_storage(storage)
    , m_config(config)
    , m_throttle(throttle ? std::move(throttle) : std::make_shared<LoginThrottle>())
{
}

HttpResponse Api::handle(const HttpRequest &request)
{
    QElapsedTimer timer;
    timer.start();

    HttpResponse response;
    const QString path = request.path.endsWith(QLatin1Char('/')) && request.path.size() > 1
                             ? request.path.chopped(1)
                             : request.path;
    const QByteArray &method = request.method;

    const auto needs = [&](const char *expected) { return method == expected; };

    // An unknown path is 404 and a known one asked the wrong way is 405 -
    // before the token is even looked at: neither is a question of who asks.
    const auto route = routes().constFind(path);
    if (route == routes().constEnd()) {
        response = error(404, QStringLiteral("not_found"), QStringLiteral("Неизвестный запрос"));
    } else if (!route->contains(method)) {
        response = error(405, QStringLiteral("method"), QStringLiteral("Метод не поддерживается"));
        QByteArrayList allowed = *route;
        response.headers.insert("Allow", allowed.join(", "));
    } else if (path == QLatin1String("/api/ping") || path == QLatin1String("/")) {
        response = ping();
    } else if (path == QLatin1String("/api/register")) {
        response = registerUser(request);
    } else if (path == QLatin1String("/api/login")) {
        response = login(request);
    } else {
        const qint64 userId = authenticate(request);
        if (!userId) {
            response = error(401, QStringLiteral("unauthorized"),
                             QStringLiteral("Требуется вход в аккаунт"));
        } else if (path == QLatin1String("/api/logout") && needs("POST")) {
            response = logout(request);
        } else if (path == QLatin1String("/api/me") && needs("GET")) {
            response = me(userId);
        } else if (path == QLatin1String("/api/profile") && needs("POST")) {
            response = profile(request, userId);
        } else if (path == QLatin1String("/api/password") && needs("POST")) {
            response = changePassword(request, userId);
        } else if (path == QLatin1String("/api/sessions") && needs("GET")) {
            response = sessions(request, userId);
        } else if (path == QLatin1String("/api/sessions/revoke") && needs("POST")) {
            response = revokeSession(request, userId);
        } else if (path == QLatin1String("/api/sync") && needs("POST")) {
            response = sync(request, userId);
        } else if (path == QLatin1String("/api/record") && needs("GET")) {
            response = record(request, userId);
        } else if (path == QLatin1String("/api/share") && needs("GET")) {
            response = shareRead(request, userId);
        } else if (path == QLatin1String("/api/share") && needs("POST")) {
            response = shareConfigure(request, userId);
        } else if (path == QLatin1String("/api/share/member") && needs("POST")) {
            response = shareMember(request, userId);
        } else if (path == QLatin1String("/api/share/link") && needs("POST")) {
            response = shareLink(request, userId);
        } else if (path == QLatin1String("/api/share/peek") && needs("GET")) {
            response = sharePeek(request, userId);
        } else if (path == QLatin1String("/api/share/join") && needs("POST")) {
            response = shareJoin(request, userId);
        } else if (path == QLatin1String("/api/share/leave") && needs("POST")) {
            response = shareLeave(request, userId);
        }
    }

    // The headers of the GET, Content-Length included, and no body.
    if (method == "HEAD")
        response.headOnly = true;

    // Behind a trusted proxy the client, and the proxy it came through.
    const QString from = request.client.isNull() || request.client == request.peer
                             ? request.peer.toString()
                             : QStringLiteral("%1 через %2").arg(request.client.toString(),
                                                               request.peer.toString());
    qInfo().noquote() << QStringLiteral("%1 %2 %3 -> %4 (%5 мс)")
                             .arg(from, QString::fromLatin1(method), path)
                             .arg(response.status)
                             .arg(timer.elapsed());
    return response;
}

// ----------------------------------------------------------------- helpers

HttpResponse Api::error(int status, const QString &code, const QString &message)
{
    QJsonObject object;
    object.insert(QStringLiteral("error"), code);
    object.insert(QStringLiteral("message"), message);
    return HttpResponse::json(status, QJsonDocument(object).toJson(QJsonDocument::Compact));
}

HttpResponse Api::ok(const QJsonObject &object, int status)
{
    return HttpResponse::json(status, QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool Api::parseBody(const HttpRequest &request, QJsonObject *object, HttpResponse *failure)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(request.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *failure = error(400, QStringLiteral("bad_json"),
                         QStringLiteral("Тело запроса — не объект JSON"));
        return false;
    }
    *object = document.object();
    return true;
}

QString Api::hashPassword(const QString &password, int iterations)
{
    const QByteArray salt = randomBytes(16);
    const QByteArray key = QPasswordDigestor::deriveKeyPbkdf2(
        QCryptographicHash::Sha256, password.toUtf8(), salt, iterations, 32);
    return QStringLiteral("pbkdf2-sha256$%1$%2$%3")
        .arg(iterations)
        .arg(QString::fromLatin1(salt.toBase64()), QString::fromLatin1(key.toBase64()));
}

bool Api::verifyPassword(const QString &password, const QString &stored)
{
    const QStringList parts = stored.split(QLatin1Char('$'));
    if (parts.size() != 4 || parts.at(0) != QLatin1String("pbkdf2-sha256"))
        return false;
    bool ok = false;
    const int iterations = parts.at(1).toInt(&ok);
    if (!ok || iterations <= 0)
        return false;
    const QByteArray salt = QByteArray::fromBase64(parts.at(2).toLatin1());
    const QByteArray expected = QByteArray::fromBase64(parts.at(3).toLatin1());
    const QByteArray key = QPasswordDigestor::deriveKeyPbkdf2(
        QCryptographicHash::Sha256, password.toUtf8(), salt, iterations, expected.size());
    return sameBytes(key, expected);
}

QString Api::loginProblem(const QString &login)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9._@-]{3,64}$"));
    if (!pattern.match(login).hasMatch()) {
        return QStringLiteral("Логин: от 3 до 64 символов — латинские буквы, цифры, "
                              "точка, дефис, подчёркивание или @");
    }
    return QString();
}

QString Api::passwordProblem(const QString &password)
{
    if (password.size() < 6)
        return QStringLiteral("Пароль должен быть не короче 6 символов");
    if (password.size() > 256)
        return QStringLiteral("Пароль слишком длинный");
    return QString();
}

qint64 Api::authenticate(const HttpRequest &request, QByteArray *tokenHashOut)
{
    // The scheme is case-insensitive (RFC 7235): "bearer" from a proxy or a
    // script is as good as "Bearer".
    const QByteArray header = request.header("authorization");
    if (header.size() < 7 || header.left(7).toLower() != "bearer ")
        return 0;
    const QString token = QString::fromLatin1(header.mid(7).trimmed());
    if (token.isEmpty())
        return 0;
    const QByteArray hash = tokenHash(token);
    if (tokenHashOut)
        *tokenHashOut = hash;
    return m_storage->userForToken(hash, m_config.tokenLifetimeDays);
}

QString Api::issueToken(Storage &storage, qint64 userId, const QString &device)
{
    const QString token = QString::fromLatin1(
        randomBytes(32).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    if (!storage.addToken(tokenHash(token), userId, device))
        return QString();
    return token;
}

QJsonObject Api::userJson(qint64 userId)
{
    const UserInfo user = m_storage->userById(userId);
    QJsonObject object;
    object.insert(QStringLiteral("id"), QString::number(user.id));
    object.insert(QStringLiteral("login"), user.login);
    object.insert(QStringLiteral("displayName"), user.displayName);
    object.insert(QStringLiteral("createdAt"), user.createdAt.toString(Qt::ISODate));
    return object;
}

QString Api::loginKey(const QHostAddress &client, const QString &login)
{
    return QStringLiteral("login %1 %2").arg(client.toString(), login.toLower());
}

QString Api::peerKey(const QHostAddress &client)
{
    return QStringLiteral("peer %1").arg(client.toString());
}

bool Api::isThrottled(const QString &key, int limit)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QMutexLocker locker(&m_throttle->lock);
    auto it = m_throttle->failures.find(key);
    if (it == m_throttle->failures.end())
        return false;
    QList<qint64> &times = it.value();
    times.erase(std::remove_if(times.begin(), times.end(),
                               [now](qint64 at) { return now - at > kFailureWindowMs; }),
                times.end());
    if (times.isEmpty()) {
        m_throttle->failures.erase(it);
        return false;
    }
    return times.size() >= limit;
}

void Api::noteFailure(const QString &key)
{
    QMutexLocker locker(&m_throttle->lock);
    m_throttle->failures[key].append(QDateTime::currentMSecsSinceEpoch());
    // Keys of addresses and logins nobody tries again would stay for ever.
    if (++m_throttle->noted % 256 == 0)
        forgetOldFailures();
}

void Api::forgetOldFailures()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QHash<QString, QList<qint64>> &failures = m_throttle->failures;
    for (auto it = failures.begin(); it != failures.end();) {
        QList<qint64> &times = it.value();
        times.erase(std::remove_if(times.begin(), times.end(),
                                   [now](qint64 at) { return now - at > kFailureWindowMs; }),
                    times.end());
        if (times.isEmpty())
            it = failures.erase(it);
        else
            ++it;
    }
}

// ------------------------------------------------------------------ routes

HttpResponse Api::ping() const
{
    QJsonObject object;
    object.insert(QStringLiteral("service"), QStringLiteral("calendar-planner-server"));
    object.insert(QStringLiteral("version"), QStringLiteral(SERVER_VERSION));
    object.insert(QStringLiteral("api"), apiVersion());
    object.insert(QStringLiteral("status"), QStringLiteral("ok"));
    object.insert(QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("registration"), m_config.allowRegistration);
    return ok(object);
}

HttpResponse Api::registerUser(const HttpRequest &request)
{
    if (!m_config.allowRegistration) {
        return error(403, QStringLiteral("registration_closed"),
                     QStringLiteral("Регистрация на этом сервере отключена"));
    }

    QJsonObject body;
    HttpResponse failure;
    if (!parseBody(request, &body, &failure))
        return failure;

    const QString login = body.value(QStringLiteral("login")).toString().trimmed();
    const QString password = body.value(QStringLiteral("password")).toString();
    QString displayName = body.value(QStringLiteral("displayName")).toString().trimmed().left(100);
    if (displayName.isEmpty())
        displayName = login;

    QString problem = loginProblem(login);
    if (problem.isEmpty())
        problem = passwordProblem(password);
    if (!problem.isEmpty())
        return error(400, QStringLiteral("invalid"), problem);

    if (m_storage->userByLogin(login).isValid()) {
        return error(409, QStringLiteral("login_taken"),
                     QStringLiteral("Логин «%1» уже занят").arg(login));
    }

    // The hash is the slow part and needs no base: it is made before the
    // write, on this thread, while other threads write.
    const QString hash = hashPassword(password, m_config.passwordIterations);
    const QString device = body.value(QStringLiteral("device")).toString();
    QString token;
    UserInfo user;
    // The account and its first session together. The work may run on the
    // commit thread: it touches only the storage it is given.
    const bool created = m_storage->transact([&](Storage &storage) {
        user = storage.createUser(login, displayName, hash);
        if (user.isValid())
            token = issueToken(storage, user.id, device);
        return user.isValid() && !token.isEmpty();
    });
    if (!created) {
        // Another thread registered the same login a moment earlier.
        if (m_storage->userByLogin(login).isValid()) {
            return error(409, QStringLiteral("login_taken"),
                         QStringLiteral("Логин «%1» уже занят").arg(login));
        }
        return error(500, QStringLiteral("storage"),
                     QStringLiteral("Не удалось создать аккаунт"));
    }

    QJsonObject object;
    object.insert(QStringLiteral("token"), token);
    object.insert(QStringLiteral("user"), userJson(user.id));
    qInfo().noquote() << "Зарегистрирован пользователь" << user.login;
    return ok(object, 201);
}

HttpResponse Api::login(const HttpRequest &request)
{
    QJsonObject body;
    HttpResponse failure;
    if (!parseBody(request, &body, &failure))
        return failure;

    const QString login = body.value(QStringLiteral("login")).toString().trimmed();
    const QString password = body.value(QStringLiteral("password")).toString();
    if (isThrottled(loginKey(request.client, login), kMaxFailures)
        || isThrottled(peerKey(request.client), kMaxAddressFailures)) {
        return error(429, QStringLiteral("throttled"),
                     QStringLiteral("Слишком много неудачных попыток. Попробуйте через "
                                    "несколько минут"));
    }

    const UserInfo user = m_storage->userByLogin(login);
    if (!user.isValid() || !verifyPassword(password, m_storage->passwordHash(user.id))) {
        noteFailure(loginKey(request.client, login));
        noteFailure(peerKey(request.client));
        return error(401, QStringLiteral("wrong_credentials"),
                     QStringLiteral("Неверный логин или пароль"));
    }

    const QString token =
        issueToken(*m_storage, user.id, body.value(QStringLiteral("device")).toString());
    if (token.isEmpty())
        return error(500, QStringLiteral("storage"), QStringLiteral("Не удалось начать сеанс"));

    QJsonObject object;
    object.insert(QStringLiteral("token"), token);
    object.insert(QStringLiteral("user"), userJson(user.id));
    return ok(object);
}

HttpResponse Api::logout(const HttpRequest &request)
{
    QByteArray hash;
    authenticate(request, &hash);
    m_storage->removeToken(hash);
    return ok(QJsonObject());
}

HttpResponse Api::me(qint64 userId)
{
    const UserInfo user = m_storage->userById(userId);
    QJsonObject object;
    object.insert(QStringLiteral("user"), userJson(userId));
    object.insert(QStringLiteral("revision"), user.revision);
    object.insert(QStringLiteral("records"), m_storage->recordCount(userId));
    object.insert(QStringLiteral("sessions"), m_storage->tokenCount(userId));
    return ok(object);
}

HttpResponse Api::profile(const HttpRequest &request, qint64 userId)
{
    QJsonObject body;
    HttpResponse failure;
    if (!parseBody(request, &body, &failure))
        return failure;
    const QString name = body.value(QStringLiteral("displayName")).toString().trimmed().left(100);
    if (name.isEmpty())
        return error(400, QStringLiteral("invalid"), QStringLiteral("Имя не может быть пустым"));
    m_storage->setDisplayName(userId, name);
    QJsonObject object;
    object.insert(QStringLiteral("user"), userJson(userId));
    return ok(object);
}

HttpResponse Api::changePassword(const HttpRequest &request, qint64 userId)
{
    QJsonObject body;
    HttpResponse failure;
    if (!parseBody(request, &body, &failure))
        return failure;

    const QString oldPassword = body.value(QStringLiteral("oldPassword")).toString();
    const QString newPassword = body.value(QStringLiteral("newPassword")).toString();
    // A token in the wrong hands must not become a way to guess the password
    // it was issued for.
    const QString account = QStringLiteral("password %1").arg(userId);
    if (isThrottled(account, kMaxFailures)) {
        return error(429, QStringLiteral("throttled"),
                     QStringLiteral("Слишком много неудачных попыток. Попробуйте через "
                                    "несколько минут"));
    }
    if (!verifyPassword(oldPassword, m_storage->passwordHash(userId))) {
        noteFailure(account);
        return error(403, QStringLiteral("wrong_credentials"),
                     QStringLiteral("Текущий пароль указан неверно"));
    }
    const QString problem = passwordProblem(newPassword);
    if (!problem.isEmpty())
        return error(400, QStringLiteral("invalid"), problem);

    const QString stored = hashPassword(newPassword, m_config.passwordIterations);
    QByteArray hash;
    authenticate(request, &hash);
    // The new password and the end of the other sessions go together.
    const bool changed = m_storage->transact([&](Storage &storage) {
        return storage.setPasswordHash(userId, stored) && storage.removeOtherTokens(userId, hash);
    });
    if (!changed)
        return error(500, QStringLiteral("storage"), QStringLiteral("Не удалось сменить пароль"));
    return ok(QJsonObject());
}

HttpResponse Api::sessions(const HttpRequest &request, qint64 userId)
{
    QByteArray own;
    authenticate(request, &own);
    QJsonArray list;
    for (const SessionInfo &session : m_storage->sessions(userId, m_config.tokenLifetimeDays)) {
        QJsonObject entry;
        entry.insert(QStringLiteral("id"), sessionId(session.tokenHash));
        entry.insert(QStringLiteral("device"), session.device);
        entry.insert(QStringLiteral("createdAt"), session.createdAt.toString(Qt::ISODate));
        entry.insert(QStringLiteral("lastUsedAt"), session.lastUsedAt.toString(Qt::ISODate));
        entry.insert(QStringLiteral("current"), session.tokenHash == own);
        list.append(entry);
    }
    QJsonObject object;
    object.insert(QStringLiteral("sessions"), list);
    return ok(object);
}

HttpResponse Api::revokeSession(const HttpRequest &request, qint64 userId)
{
    QJsonObject body;
    HttpResponse failure;
    if (!parseBody(request, &body, &failure))
        return failure;
    const QString id = body.value(QStringLiteral("id")).toString();
    if (id.isEmpty())
        return error(400, QStringLiteral("invalid"), QStringLiteral("Не указан сеанс"));

    // Looked for among the sessions of this account only: the id of somebody
    // else's session finds nothing, and looks exactly like one that has ended.
    for (const SessionInfo &session : m_storage->sessions(userId, m_config.tokenLifetimeDays)) {
        if (sessionId(session.tokenHash) != id)
            continue;
        if (!m_storage->removeToken(session.tokenHash)) {
            return error(500, QStringLiteral("storage"),
                         QStringLiteral("Не удалось завершить сеанс"));
        }
        return ok(QJsonObject());
    }
    return error(404, QStringLiteral("not_found"), QStringLiteral("Такого сеанса уже нет"));
}

HttpResponse Api::sync(const HttpRequest &request, qint64 userId)
{
    QJsonObject body;
    HttpResponse failure;
    if (!parseBody(request, &body, &failure))
        return failure;

    const qint64 since = qint64(body.value(QStringLiteral("since")).toDouble(0));
    const QString device = body.value(QStringLiteral("device")).toString();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int clamped = 0;

    QList<SyncRecord> incoming;
    const QJsonArray changes = body.value(QStringLiteral("changes")).toArray();
    incoming.reserve(changes.size());
    for (const QJsonValue &value : changes) {
        const QJsonObject change = value.toObject();
        SyncRecord record;
        record.kind = change.value(QStringLiteral("kind")).toString();
        record.id = change.value(QStringLiteral("id")).toString();
        record.deleted = change.value(QStringLiteral("deleted")).toBool();
        record.restore = change.value(QStringLiteral("restore")).toBool(false);
        record.modified = qint64(change.value(QStringLiteral("modified")).toDouble(0));
        if (!validKind(record.kind) || record.id.isEmpty() || record.id.size() > 200
            || record.modified <= 0) {
            return error(400, QStringLiteral("invalid"),
                         QStringLiteral("Неверная запись в списке изменений"));
        }
        // The clock of the device is ahead: the change counts as made now.
        if (record.modified > now + kFutureToleranceMs) {
            record.modified = now;
            ++clamped;
        }
        if (!record.deleted) {
            const QJsonValue data = change.value(QStringLiteral("data"));
            if (!data.isObject()) {
                return error(400, QStringLiteral("invalid"),
                             QStringLiteral("У записи %1 нет данных").arg(record.key()));
            }
            record.data = QJsonDocument(data.toObject()).toJson(QJsonDocument::Compact);
        }
        incoming.append(record);
    }

    if (clamped > 0) {
        qWarning().noquote() << QStringLiteral("%1: часы устройства %2 спешат, время правки "
                                               "записей (%3) взято по серверу")
                                    .arg(request.client.toString(),
                                         device.isEmpty() ? QStringLiteral("без имени")
                                                          : QStringLiteral("«%1»").arg(device))
                                    .arg(clamped);
    }

    const SyncOutcome outcome = m_storage->sync(userId, since, incoming, device);
    if (!outcome.ok) {
        return error(500, QStringLiteral("storage"),
                     QStringLiteral("Сервер не смог сохранить изменения"));
    }

    QJsonArray outgoing;
    for (const SyncRecord &record : outcome.changes) {
        QJsonObject change;
        change.insert(QStringLiteral("kind"), record.kind);
        change.insert(QStringLiteral("id"), record.id);
        change.insert(QStringLiteral("deleted"), record.deleted);
        change.insert(QStringLiteral("modified"), double(record.modified));
        change.insert(QStringLiteral("revision"), double(record.revision));
        if (!record.deleted)
            change.insert(QStringLiteral("data"), QJsonDocument::fromJson(record.data).object());
        outgoing.append(change);
    }

    // What could not be applied to a shared calendar, with enough of the change
    // left in it for the planner to offer bringing the record back.
    QJsonArray conflicts;
    for (const ShareConflict &conflict : outcome.conflicts) {
        QJsonObject item;
        item.insert(QStringLiteral("kind"), conflict.kind);
        item.insert(QStringLiteral("id"), conflict.id);
        item.insert(QStringLiteral("calendarId"), conflict.calendarId);
        item.insert(QStringLiteral("reason"), conflict.reason);
        if (!conflict.data.isEmpty())
            item.insert(QStringLiteral("data"), QJsonDocument::fromJson(conflict.data).object());
        conflicts.append(item);
    }

    QJsonObject object;
    object.insert(QStringLiteral("revision"), double(outcome.revision));
    object.insert(QStringLiteral("accepted"), outcome.accepted);
    object.insert(QStringLiteral("rejected"), outcome.rejected);
    object.insert(QStringLiteral("conflicts"), conflicts);
    object.insert(QStringLiteral("changes"), outgoing);
    object.insert(QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    const HttpResponse response = ok(object);

    // Statistics only: an exchange that went through is not failed for them.
    SyncTraffic traffic;
    traffic.changesIn = int(incoming.size());
    traffic.changesOut = int(outgoing.size());
    traffic.bytesIn = request.body.size();
    traffic.bytesOut = response.body.size();
    m_storage->noteSync(userId, traffic);
    return response;
}

HttpResponse Api::record(const HttpRequest &request, qint64 userId)
{
    const QString kind = request.query.queryItemValue(QStringLiteral("kind"));
    const QString id = request.query.queryItemValue(QStringLiteral("id"));
    if (!validKind(kind) || id.isEmpty() || id.size() > 200) {
        return error(400, QStringLiteral("invalid"),
                     QStringLiteral("Неверный запрос записи"));
    }

    // The copy this account holds: a record of a shared calendar has one per
    // member, and the fan-out of sync() keeps every one of them current.
    bool found = false;
    const SyncRecord stored = m_storage->record(userId, kind, id, &found);
    QJsonObject object;
    object.insert(QStringLiteral("found"), found);
    if (found) {
        QJsonObject change;
        change.insert(QStringLiteral("kind"), stored.kind);
        change.insert(QStringLiteral("id"), stored.id);
        change.insert(QStringLiteral("deleted"), stored.deleted);
        change.insert(QStringLiteral("modified"), double(stored.modified));
        change.insert(QStringLiteral("revision"), double(stored.revision));
        if (!stored.deleted)
            change.insert(QStringLiteral("data"), QJsonDocument::fromJson(stored.data).object());
        object.insert(QStringLiteral("change"), change);
    }
    return ok(object);
}

// ------------------------------------------------------- shared calendars ----

namespace {

/*! The share as the planner reads it: the mode, the members and their rights. */
QJsonObject shareJson(const ShareInfo &share)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), share.id);
    object.insert(QStringLiteral("calendarId"), share.calendarId);
    object.insert(QStringLiteral("enabled"), share.enabled);
    object.insert(QStringLiteral("mode"), shareModeKey(share.mode));
    object.insert(QStringLiteral("ownerLogin"), share.ownerLogin);
    object.insert(QStringLiteral("ownerName"), share.ownerDisplayName);

    QJsonArray members;
    for (const ShareMemberInfo &member : share.members) {
        QJsonObject one;
        one.insert(QStringLiteral("login"), member.login);
        one.insert(QStringLiteral("displayName"), member.displayName);
        one.insert(QStringLiteral("access"), shareAccessKey(member.access));
        one.insert(QStringLiteral("joinedAt"), member.joinedAt.toString(Qt::ISODate));
        members.append(one);
    }
    object.insert(QStringLiteral("members"), members);
    return object;
}

} // namespace

HttpResponse Api::shareRead(const HttpRequest &request, qint64 userId)
{
    const QString calendarId = request.query.queryItemValue(QStringLiteral("calendar"));
    if (calendarId.isEmpty()) {
        return error(400, QStringLiteral("invalid"),
                     QStringLiteral("Не указан календарь"));
    }

    const ShareInfo share = m_storage->shares().shareOf(userId, calendarId);
    QJsonObject object;
    object.insert(QStringLiteral("share"),
                  share.isValid() ? shareJson(share) : QJsonObject());
    return ok(object);
}

HttpResponse Api::shareConfigure(const HttpRequest &request, qint64 userId)
{
    QJsonObject body;
    HttpResponse failure;
    if (!parseBody(request, &body, &failure))
        return failure;

    const QString calendarId = body.value(QStringLiteral("calendar")).toString();
    if (calendarId.isEmpty() || calendarId.size() > 200) {
        return error(400, QStringLiteral("invalid"),
                     QStringLiteral("Не указан календарь"));
    }
    const bool enabled = body.value(QStringLiteral("enabled")).toBool(true);
    const ShareMode mode = shareModeFromKey(body.value(QStringLiteral("mode")).toString());

    QString problem;
    ShareInfo share;
    const bool configured = m_storage->transact([&](Storage &storage) {
        share = storage.shares().configure(userId, calendarId, enabled, mode, &problem);
        return share.isValid();
    });
    if (!configured) {
        return error(400, QStringLiteral("share"),
                     problem.isEmpty() ? m_storage->lastError() : problem);
    }

    QJsonObject object;
    object.insert(QStringLiteral("share"), shareJson(share));
    return ok(object);
}

HttpResponse Api::shareMember(const HttpRequest &request, qint64 userId)
{
    QJsonObject body;
    HttpResponse failure;
    if (!parseBody(request, &body, &failure))
        return failure;

    const QString calendarId = body.value(QStringLiteral("calendar")).toString();
    const QString login = body.value(QStringLiteral("login")).toString().trimmed();
    if (calendarId.isEmpty() || login.isEmpty()) {
        return error(400, QStringLiteral("invalid"),
                     QStringLiteral("Нужны календарь и логин участника"));
    }

    // What the work found out, for the answer: the share is looked up inside
    // the transaction, so that it cannot change between the look and the write.
    enum class Outcome { Done, NoShare, NoMember, Refused, Failed };
    Outcome outcome = Outcome::Failed;
    QString problem;
    const bool remove = body.value(QStringLiteral("remove")).toBool(false);
    const ShareAccess access = shareAccessFromKey(body.value(QStringLiteral("access")).toString());
    m_storage->transact([&](Storage &storage) {
        const ShareInfo share = storage.shares().shareOf(userId, calendarId);
        if (!share.isValid()) {
            outcome = Outcome::NoShare;
            return false;
        }
        ShareMemberInfo target;
        for (const ShareMemberInfo &member : share.members) {
            if (member.login.compare(login, Qt::CaseInsensitive) == 0)
                target = member;
        }
        if (target.userId == 0) {
            outcome = Outcome::NoMember;
            return false;
        }
        if (remove) {
            // The owner shows a member the door: the copies go with the access,
            // or the records would stay on a device that may no longer see them.
            qint64 revision = 0;
            if (!storage.shares().dropCopies(target.userId, target.twinId, &revision)
                || !storage.shares().removeMember(share.id, target.userId, &problem)) {
                if (problem.isEmpty())
                    problem = storage.shares().lastError();
                outcome = Outcome::Failed;
                return false;
            }
        } else if (!storage.shares().setMemberAccess(userId, calendarId, login, access, &problem)) {
            outcome = Outcome::Refused;
            return false;
        }
        outcome = Outcome::Done;
        return true;
    });
    switch (outcome) {
    case Outcome::Done:
        break;
    case Outcome::NoShare:
        return error(404, QStringLiteral("not_found"),
                     QStringLiteral("Этот календарь никому не открыт"));
    case Outcome::NoMember:
        return error(404, QStringLiteral("not_found"),
                     QStringLiteral("Участник «%1» не найден").arg(login));
    case Outcome::Refused:
        return error(400, QStringLiteral("share"), problem);
    case Outcome::Failed:
        return error(500, QStringLiteral("share"),
                     problem.isEmpty() ? m_storage->lastError() : problem);
    }

    QJsonObject object;
    object.insert(QStringLiteral("share"),
                  shareJson(m_storage->shares().shareOf(userId, calendarId)));
    return ok(object);
}

HttpResponse Api::shareLink(const HttpRequest &request, qint64 userId)
{
    QJsonObject body;
    HttpResponse failure;
    if (!parseBody(request, &body, &failure))
        return failure;

    const QString calendarId = body.value(QStringLiteral("calendar")).toString();
    const bool singleUse = body.value(QStringLiteral("singleUse")).toBool(true);
    const qint64 life = qint64(body.value(QStringLiteral("lifeSeconds")).toDouble(3600));
    if (life < 0 || life > 3600LL * 24 * 366) {
        return error(400, QStringLiteral("invalid"),
                     QStringLiteral("Неверный срок действия ссылки"));
    }

    QString problem;
    QString token;
    const bool created = m_storage->transact([&](Storage &storage) {
        token = storage.shares().createLink(userId, calendarId, singleUse, life, &problem);
        return !token.isEmpty();
    });
    if (!created) {
        return error(400, QStringLiteral("share"),
                     problem.isEmpty() ? m_storage->lastError() : problem);
    }

    QJsonObject object;
    object.insert(QStringLiteral("token"), token);
    object.insert(QStringLiteral("singleUse"), singleUse);
    object.insert(QStringLiteral("lifeSeconds"), double(life));
    if (life > 0) {
        object.insert(QStringLiteral("expiresAt"),
                      QDateTime::currentDateTimeUtc().addSecs(life).toString(Qt::ISODate));
    }
    return ok(object, 201);
}

HttpResponse Api::sharePeek(const HttpRequest &request, qint64 userId)
{
    const QString token = request.query.queryItemValue(QStringLiteral("token"));
    QString problem;
    const ShareInfo share = m_storage->shares().resolveLink(token, &problem);
    if (!share.isValid())
        return error(404, QStringLiteral("link"), problem);
    if (share.ownerId == userId) {
        return error(409, QStringLiteral("own_calendar"),
                     QStringLiteral("Это ваш собственный календарь"));
    }

    // What the add dialog fills itself in with: the calendar as the owner has
    // it right now, so the subscriber starts from its name and its colour.
    QJsonObject object;
    object.insert(QStringLiteral("share"), shareJson(share));
    const QByteArray data = m_storage->recordData(share.ownerId, QStringLiteral("calendar"),
                                                  share.calendarId);
    if (!data.isEmpty())
        object.insert(QStringLiteral("calendar"), QJsonDocument::fromJson(data).object());
    return ok(object);
}

HttpResponse Api::shareJoin(const HttpRequest &request, qint64 userId)
{
    QJsonObject body;
    HttpResponse failure;
    if (!parseBody(request, &body, &failure))
        return failure;

    const QString token = body.value(QStringLiteral("token")).toString();
    const QString twinId = body.value(QStringLiteral("twinId")).toString();
    if (twinId.isEmpty() || twinId.size() > 200) {
        return error(400, QStringLiteral("invalid"),
                     QStringLiteral("Не указан календарь, в который добавить"));
    }

    QString problem;
    qint64 revision = 0;
    ShareInfo share;
    // The copies, the membership and the spent link go in together - and a
    // single use link cannot be walked through twice on two threads.
    const bool joined = m_storage->transact([&](Storage &storage) {
        share = storage.shares().join(token, userId, twinId, &revision, &problem);
        if (!share.isValid())
            return false;
        // A single use link is spent only once somebody really walked through it.
        storage.shares().spendLink(token);
        return true;
    });
    if (!joined) {
        if (!share.isValid() && !problem.isEmpty())
            return error(400, QStringLiteral("link"), problem);
        return error(500, QStringLiteral("link"), m_storage->lastError());
    }

    ShareAccess access = ShareAccess::Read;
    for (const ShareMemberInfo &member : share.members) {
        if (member.userId == userId)
            access = member.access;
    }

    QJsonObject object;
    object.insert(QStringLiteral("share"), shareJson(share));
    object.insert(QStringLiteral("access"), shareAccessKey(access));
    object.insert(QStringLiteral("revision"), double(revision));
    qInfo().noquote() << QStringLiteral("Календарь %1 открыт пользователю %2")
                             .arg(share.calendarId)
                             .arg(userId);
    return ok(object, 201);
}

HttpResponse Api::shareLeave(const HttpRequest &request, qint64 userId)
{
    QJsonObject body;
    HttpResponse failure;
    if (!parseBody(request, &body, &failure))
        return failure;

    const QString twinId = body.value(QStringLiteral("twinId")).toString();
    // Giving the access up takes the copies with it and takes the member out of
    // the owner's list, which is the same thing seen from the other side.
    bool found = false;
    qint64 revision = 0;
    QString problem;
    const bool left = m_storage->transact([&](Storage &storage) {
        const ShareInfo share = storage.shares().shareOfTwin(userId, twinId);
        found = share.isValid();
        if (!found)
            return false;
        if (!storage.shares().dropCopies(userId, twinId, &revision)
            || !storage.shares().removeMember(share.id, userId, &problem)) {
            if (problem.isEmpty())
                problem = storage.shares().lastError();
            return false;
        }
        return true;
    });
    if (!found) {
        return error(404, QStringLiteral("not_found"),
                     QStringLiteral("Этот календарь вам никто не открывал"));
    }
    if (!left) {
        return error(500, QStringLiteral("share"),
                     problem.isEmpty() ? m_storage->lastError() : problem);
    }

    QJsonObject object;
    object.insert(QStringLiteral("revision"), double(revision));
    return ok(object);
}
