#pragma once

#include "config.h"
#include "httpserver.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMutex>

#include <memory>

class Storage;

/*!
 * The wrong passwords the server remembers, shared by the Api of every
 * worker thread: a guess counts wherever it lands.
 */
struct LoginThrottle
{
    QMutex lock;
    QHash<QString, QList<qint64>> failures;
    int noted = 0;
};

/*!
 * The JSON API of the server. Every answer is a JSON object; a failure has
 * "error" (a stable code for programs) and "message" (Russian, for people).
 *
 *   GET  /api/ping      health check, needs no account
 *   POST /api/register  {login, password, displayName?, device?} -> {token, user}
 *   POST /api/login     {login, password, device?}               -> {token, user}
 *   POST /api/logout    ends the session of the token
 *   GET  /api/me        the account and what is stored for it
 *   POST /api/profile   {displayName}
 *   POST /api/password  {oldPassword, newPassword}, logs out the other devices
 *   GET  /api/sessions         the sessions of the account, no key or hash among them
 *   POST /api/sessions/revoke  {id} ends one of them
 *   POST /api/sync      {since, device?, changes: [...]} -> {revision, changes: [...]}
 *
 * Shared calendars (see shares.h). Everything here is the owner's, except the
 * three a member uses to walk through a link and to leave again:
 *
 *   GET  /api/share?calendar=<id>   what the owner set up, with the members
 *   POST /api/share     {calendar, enabled, mode}
 *   POST /api/share/member  {calendar, login, access} or {calendar, login, remove}
 *   POST /api/share/link    {calendar, singleUse, lifeSeconds} -> {token, expiresAt}
 *   GET  /api/share/peek?token=<t>  what the link leads to, before joining
 *   POST /api/share/join    {token, twinId} -> the share, records copied over
 *   POST /api/share/leave   {twinId}        gives the access up
 *
 * Everything but ping, register and login needs "Authorization: Bearer <token>".
 */
class Api
{
public:
    static constexpr int apiVersion() { return 1; }

    /*!
     * One Api per thread, on the Storage of that thread. @p throttle is the
     * memory of wrong passwords the threads share; without one the Api keeps
     * its own.
     */
    Api(Storage *storage, const ServerConfig &config,
        std::shared_ptr<LoginThrottle> throttle = nullptr);

    HttpResponse handle(const HttpRequest &request);

    /*! "pbkdf2-sha256$rounds$salt$hash", salted with random bytes. */
    static QString hashPassword(const QString &password, int iterations);
    static bool verifyPassword(const QString &password, const QString &stored);

    /*! The rules a login and a password have to follow; empty when fine. */
    static QString loginProblem(const QString &login);
    static QString passwordProblem(const QString &password);

private:
    HttpResponse ping() const;
    HttpResponse registerUser(const HttpRequest &request);
    HttpResponse login(const HttpRequest &request);
    HttpResponse logout(const HttpRequest &request);
    HttpResponse me(qint64 userId);
    HttpResponse profile(const HttpRequest &request, qint64 userId);
    HttpResponse changePassword(const HttpRequest &request, qint64 userId);
    HttpResponse sessions(const HttpRequest &request, qint64 userId);
    HttpResponse revokeSession(const HttpRequest &request, qint64 userId);
    HttpResponse sync(const HttpRequest &request, qint64 userId);

    /*!
     * GET /api/record?kind=…&id=… - one record as this account has it, for
     * the planner that is about to edit it and wants the newest version first
     * without waiting for a whole exchange. The cursor is not involved.
     */
    HttpResponse record(const HttpRequest &request, qint64 userId);

    // ---- shared calendars --------------------------------------------------
    HttpResponse shareRead(const HttpRequest &request, qint64 userId);
    HttpResponse shareConfigure(const HttpRequest &request, qint64 userId);
    HttpResponse shareMember(const HttpRequest &request, qint64 userId);
    HttpResponse shareLink(const HttpRequest &request, qint64 userId);
    HttpResponse sharePeek(const HttpRequest &request, qint64 userId);
    HttpResponse shareJoin(const HttpRequest &request, qint64 userId);
    HttpResponse shareLeave(const HttpRequest &request, qint64 userId);

    /*! The user behind the bearer token, 0 when there is none or it is not valid. */
    qint64 authenticate(const HttpRequest &request, QByteArray *tokenHashOut = nullptr);

    /*!
     * A new token for the user, stored as a hash through @p storage (the one
     * of a transaction, Storage::transact()); the plain text goes to the
     * device only.
     */
    static QString issueToken(Storage &storage, qint64 userId, const QString &device);
    QJsonObject userJson(qint64 userId);

    /*!
     * Wrong passwords lock out what they were tried on for a while: one login
     * from one address, and an address that tries many logins at once. Not the
     * address as a whole for the first wrong password of somebody: behind a
     * reverse proxy every device has the same address, and one person's typos
     * would lock everybody out. The address is the request's client - behind
     * a trusted proxy the one it forwarded the request for.
     */
    bool isThrottled(const QString &key, int limit);
    void noteFailure(const QString &key);
    void forgetOldFailures();   //!< with the lock of m_throttle held
    static QString loginKey(const QHostAddress &client, const QString &login);
    static QString peerKey(const QHostAddress &client);

    static HttpResponse error(int status, const QString &code, const QString &message);
    static HttpResponse ok(const QJsonObject &object, int status = 200);
    static bool parseBody(const HttpRequest &request, QJsonObject *object, HttpResponse *failure);

    Storage *m_storage = nullptr;
    ServerConfig m_config;
    std::shared_ptr<LoginThrottle> m_throttle;
};
