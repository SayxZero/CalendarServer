#include "basemaker.h"

#include "api.h"
#include "shares.h"
#include "storage.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>

#include <iterator>

namespace {

/*! Bumped when what prepareBase() makes changes: an older base is made anew. */
const int kBaseFormat = 1;

const QString kPassword = QStringLiteral("secret1");

QString randomText(QRandomGenerator &random, int length)
{
    static const QString words[] = {
        QStringLiteral("встреча"),    QStringLiteral("отчёт"), QStringLiteral("купить"),
        QStringLiteral("позвонить"),  QStringLiteral("врач"),  QStringLiteral("проект"),
        QStringLiteral("тренировка"), QStringLiteral("день"),  QStringLiteral("рождения"),
        QStringLiteral("план"),       QStringLiteral("молоко"), QStringLiteral("забрать"),
    };
    QString text;
    while (text.size() < length) {
        if (!text.isEmpty())
            text += QLatin1Char(' ');
        text += words[random.bounded(int(std::size(words)))];
    }
    return text.left(length);
}

/*! One made-up record of a planner, about the size of a real one. */
SyncRecord madeUpRecord(QRandomGenerator &random, const QString &kind, const QString &id,
                        const QString &calendar, qint64 modified)
{
    QJsonObject data;
    data.insert(QStringLiteral("id"), id);
    data.insert(QStringLiteral("calendarId"), calendar);
    data.insert(QStringLiteral("title"), randomText(random, 10 + int(random.bounded(40))));
    const QDateTime start = QDateTime::currentDateTimeUtc().addDays(int(random.bounded(400)) - 200);
    if (kind == QLatin1String("event")) {
        data.insert(QStringLiteral("start"), start.toString(Qt::ISODate));
        data.insert(QStringLiteral("end"), start.addSecs(3600).toString(Qt::ISODate));
        data.insert(QStringLiteral("allDay"), random.bounded(5) == 0);
        data.insert(QStringLiteral("reminders"), QJsonArray { 10, 60 });
    } else if (kind == QLatin1String("task")) {
        data.insert(QStringLiteral("due"), start.toString(Qt::ISODate));
        data.insert(QStringLiteral("priority"), int(random.bounded(3)));
    } else {
        data.insert(QStringLiteral("taskId"), QStringLiteral("t%1").arg(random.bounded(100)));
        data.insert(QStringLiteral("doneAt"), start.toString(Qt::ISODate));
    }
    if (random.bounded(3) == 0)
        data.insert(QStringLiteral("notes"), randomText(random, int(random.bounded(400))));
    SyncRecord record;
    record.kind = kind;
    record.id = id;
    record.modified = modified;
    record.data = QJsonDocument(data).toJson(QJsonDocument::Compact);
    return record;
}

QJsonObject describe(const BaseRequest &request)
{
    QJsonObject object;
    object.insert(QStringLiteral("format"), kBaseFormat);
    object.insert(QStringLiteral("accounts"), request.accounts);
    object.insert(QStringLiteral("records"), request.records);
    return object;
}

} // namespace

bool baseIsReady(const BaseRequest &request)
{
    const QDir dir(request.dir);
    QFile file(dir.filePath(QStringLiteral("base.json")));
    if (!file.open(QIODevice::ReadOnly) || !QFile::exists(dir.filePath(QStringLiteral("tokens.json")))
        || !QFile::exists(dir.filePath(QStringLiteral("server.db")))) {
        return false;
    }
    const QJsonObject made = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject wanted = describe(request);
    for (auto it = wanted.constBegin(); it != wanted.constEnd(); ++it) {
        if (made.value(it.key()) != it.value())
            return false;
    }
    return Storage::inspect(dir.filePath(QStringLiteral("server.db"))).ok;
}

bool prepareBase(const BaseRequest &request, const std::atomic<bool> *cancel,
                 const BaseProgress &progress, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const auto say = [&](double fraction, const QString &message) {
        if (progress)
            progress(qBound(0.0, fraction, 1.0), message);
    };

    QDir().mkpath(request.dir);
    const QDir dir(request.dir);
    const QString path = dir.filePath(QStringLiteral("server.db"));
    QFile::remove(dir.filePath(QStringLiteral("base.json")));
    QString problem;
    if (!Storage::removeDatabase(path, &problem))
        return fail(problem);

    Storage storage;
    if (!storage.open(path, &problem))
        return fail(problem);
    say(0, QStringLiteral("хеш пароля"));
    QRandomGenerator random(20261026);
    // One real hash for everybody: logins in the benchmark pay the full price.
    const QString hash = Api::hashPassword(kPassword, 120000);
    QJsonArray tokens;
    QList<qint64> ids;
    for (int a = 0; a < request.accounts; ++a) {
        if (cancel && cancel->load())
            return fail(QStringLiteral("прервано"));
        say(double(a) / request.accounts,
            QStringLiteral("аккаунт %1 из %2").arg(a + 1).arg(request.accounts));
        const QString login = QStringLiteral("load%1").arg(a, 3, 10, QLatin1Char('0'));
        const UserInfo user = storage.createUser(login, QStringLiteral("Нагрузка %1").arg(a), hash);
        if (!user.isValid())
            return fail(QStringLiteral("аккаунт %1: %2").arg(login, storage.lastError()));
        ids << user.id;
        const QString token = QStringLiteral("bench-token-%1").arg(a);
        storage.addToken(QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256),
                         user.id, QStringLiteral("bench"));
        QJsonObject entry;
        entry.insert(QStringLiteral("login"), login);
        entry.insert(QStringLiteral("token"), token);
        entry.insert(QStringLiteral("records"), request.records);
        tokens.append(entry);

        QList<SyncRecord> batch;
        const qint64 base = QDateTime::currentMSecsSinceEpoch() - 86400000LL * 30;
        for (int c = 0; c < 3; ++c) {
            QJsonObject calendar;
            calendar.insert(QStringLiteral("id"), QStringLiteral("c%1").arg(c));
            calendar.insert(QStringLiteral("name"), QStringLiteral("Календарь %1").arg(c));
            SyncRecord record;
            record.kind = QStringLiteral("calendar");
            record.id = QStringLiteral("c%1").arg(c);
            record.modified = base;
            record.data = QJsonDocument(calendar).toJson(QJsonDocument::Compact);
            batch << record;
        }
        for (int r = 0; r < request.records; ++r) {
            const int roll = int(random.bounded(10));
            const QString kind = roll < 6   ? QStringLiteral("event")
                                 : roll < 9 ? QStringLiteral("task")
                                            : QStringLiteral("completion");
            batch << madeUpRecord(random, kind, QStringLiteral("e%1").arg(r),
                                  QStringLiteral("c%1").arg(random.bounded(3)), base + r);
            if (batch.size() >= 500) {
                if (!storage.sync(user.id, 0, batch, QStringLiteral("seed")).ok)
                    return fail(QStringLiteral("записи: %1").arg(storage.lastError()));
                batch.clear();
            }
        }
        if (!batch.isEmpty() && !storage.sync(user.id, 0, batch, QStringLiteral("seed")).ok)
            return fail(QStringLiteral("записи: %1").arg(storage.lastError()));
    }

    // Every tenth account shares its first calendar with the next three.
    say(1, QStringLiteral("общие календари"));
    for (int a = 0; a + 3 < ids.size(); a += 10) {
        storage.transact([&](Storage &s) {
            QString shareProblem;
            ShareStore &store = s.shares();
            if (!store.configure(ids.at(a), QStringLiteral("c0"), true, ShareMode::Edit, &shareProblem)
                     .isValid()) {
                return false;
            }
            const QString link =
                store.createLink(ids.at(a), QStringLiteral("c0"), false, 0, &shareProblem);
            for (int m = 1; m <= 3; ++m) {
                qint64 revision = 0;
                store.join(link, ids.at(a + m), QStringLiteral("twin-%1").arg(a), &revision,
                           &shareProblem);
            }
            return true;
        });
    }
    storage.close();

    QFile file(dir.filePath(QStringLiteral("tokens.json")));
    if (!file.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("не записан tokens.json"));
    file.write(QJsonDocument(tokens).toJson());
    file.close();
    QFile made(dir.filePath(QStringLiteral("base.json")));
    if (!made.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("не записан base.json"));
    made.write(QJsonDocument(describe(request)).toJson());
    return true;
}
