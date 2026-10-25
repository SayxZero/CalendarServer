#include "admindb.h"
#include "api.h"
#include "backups.h"
#include "config.h"
#include "records.h"
#include "servicecontrol.h"
#include "storage.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

/*!
 * The admin panel without its window: what it reads out of a base filled the
 * way the server fills it, the new password, the backups and the restore, the
 * command line of the service and the records of the planner.
 */
class AdminTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void usersAreSummarized();
    void calendarsShowSharesAndTwins();
    void recordsCarryTheirJson();
    void totalsCountActiveAccounts();
    void historyCountsWindows();
    void passwordChangeSignsOut();
    void backupAndRestoreKeepRevisions();
    void serviceCommandLineNamesTheBase();
    void plannerRecordsAreReadForDisplay();
    void missingServiceIsReported();

private:
    static SyncRecord record(const QString &kind, const QString &id, qint64 modified,
                             const QJsonObject &data, bool deleted = false);

    /*! Puts a row into the activity of @p userId, @p daysAgo days back. */
    void activity(qint64 userId, int daysAgo, int syncs);

    QString path() const { return m_dir->filePath(QStringLiteral("server.db")); }

    QTemporaryDir *m_dir = nullptr;
    Storage *m_storage = nullptr;
    AdminDb *m_admin = nullptr;
    UserInfo m_anna;
    UserInfo m_boris;
};

SyncRecord AdminTest::record(const QString &kind, const QString &id, qint64 modified,
                             const QJsonObject &data, bool deleted)
{
    SyncRecord result;
    result.kind = kind;
    result.id = id;
    result.modified = modified;
    result.deleted = deleted;
    if (!deleted)
        result.data = QJsonDocument(data).toJson(QJsonDocument::Compact);
    return result;
}

void AdminTest::activity(qint64 userId, int daysAgo, int syncs)
{
    QSqlQuery insert(QSqlDatabase::database(QStringLiteral("activity")));
    insert.prepare(QStringLiteral("INSERT INTO activity (user_id, day, syncs, last_at)"
                                  " VALUES (:user, :day, :syncs, :at)"));
    insert.bindValue(QStringLiteral(":user"), userId);
    insert.bindValue(QStringLiteral(":day"),
                     QDate::currentDate().addDays(-daysAgo).toString(Qt::ISODate));
    insert.bindValue(QStringLiteral(":syncs"), syncs);
    // An hour short of the day: inside its window, whatever the clock says.
    insert.bindValue(QStringLiteral(":at"),
                     QDateTime::currentDateTimeUtc().addDays(-daysAgo).addSecs(-3600).toString(Qt::ISODate));
    QVERIFY(insert.exec());
}

void AdminTest::init()
{
    m_dir = new QTemporaryDir;
    QVERIFY(m_dir->isValid());
    m_storage = new Storage;
    QString error;
    QVERIFY2(m_storage->open(path(), &error), qPrintable(error));

    const QString hash = Api::hashPassword(QStringLiteral("secret1"), 1000);
    m_anna = m_storage->createUser(QStringLiteral("anna"), QStringLiteral("Анна"), hash);
    m_boris = m_storage->createUser(QStringLiteral("boris"), QStringLiteral("Борис"), hash);
    QVERIFY(m_anna.isValid() && m_boris.isValid());
    QVERIFY(m_storage->addToken(QByteArray(32, 'a'), m_anna.id, QStringLiteral("Телефон")));
    QVERIFY(m_storage->addToken(QByteArray(32, 'b'), m_anna.id, QStringLiteral("Ноутбук")));
    QVERIFY(m_storage->addToken(QByteArray(32, 'c'), m_boris.id, QStringLiteral("Телефон")));

    QJsonObject work;
    work.insert(QStringLiteral("id"), QStringLiteral("c1"));
    work.insert(QStringLiteral("name"), QStringLiteral("Работа"));
    work.insert(QStringLiteral("color"), QStringLiteral("#3366cc"));
    QJsonObject meeting;
    meeting.insert(QStringLiteral("title"), QStringLiteral("Планёрка"));
    meeting.insert(QStringLiteral("calendarId"), QStringLiteral("c1"));
    meeting.insert(QStringLiteral("start"), QStringLiteral("2026-10-26T09:00:00"));
    QJsonObject gone = meeting;
    gone.insert(QStringLiteral("title"), QStringLiteral("Отменённое"));
    QJsonObject report;
    report.insert(QStringLiteral("title"), QStringLiteral("Отчёт"));
    report.insert(QStringLiteral("calendarId"), QStringLiteral("c1"));
    report.insert(QStringLiteral("done"), true);
    QJsonObject habit;
    habit.insert(QStringLiteral("title"), QStringLiteral("Зарядка"));
    QVERIFY(m_storage
                ->sync(m_anna.id, 0,
                       { record(QStringLiteral("calendar"), QStringLiteral("c1"), 1000, work),
                         record(QStringLiteral("event"), QStringLiteral("e1"), 1000, meeting),
                         record(QStringLiteral("event"), QStringLiteral("e2"), 1000, gone),
                         record(QStringLiteral("task"), QStringLiteral("t1"), 1000, report),
                         record(QStringLiteral("habit"), QStringLiteral("h1"), 1000, habit) },
                       QStringLiteral("Телефон"))
                .ok);
    QVERIFY(m_storage
                ->sync(m_anna.id, 0, { record(QStringLiteral("event"), QStringLiteral("e2"), 2000, {}, true) },
                       QStringLiteral("Телефон"))
                .ok);

    // Anna opens "Работа" to Boris, who keeps it as a twin of his own.
    QJsonObject twin;
    twin.insert(QStringLiteral("id"), QStringLiteral("t-work"));
    twin.insert(QStringLiteral("name"), QStringLiteral("Работа Анны"));
    QVERIFY(m_storage
                ->sync(m_boris.id, 0, { record(QStringLiteral("calendar"), QStringLiteral("t-work"), 1000, twin) },
                       QStringLiteral("Телефон"))
                .ok);
    QVERIFY(m_storage->shares().configure(m_anna.id, QStringLiteral("c1"), true, ShareMode::Read, &error)
                .isValid());
    const QString link = m_storage->shares().createLink(m_anna.id, QStringLiteral("c1"), true, 3600, &error);
    QVERIFY2(!link.isEmpty(), qPrintable(error));
    qint64 revision = 0;
    QVERIFY2(m_storage->shares().join(link, m_boris.id, QStringLiteral("t-work"), &revision, &error).isValid(),
             qPrintable(error));

    m_admin = new AdminDb;
    QVERIFY2(m_admin->open(path(), &error), qPrintable(error));
    QVERIFY(m_admin->tracksActivity());

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("activity"));
    db.setDatabaseName(path());
    QVERIFY(db.open());
}

void AdminTest::cleanup()
{
    {
        QSqlDatabase db = QSqlDatabase::database(QStringLiteral("activity"), false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("activity"));
    delete m_admin;
    delete m_storage;
    delete m_dir;
    m_admin = nullptr;
    m_storage = nullptr;
    m_dir = nullptr;
}

void AdminTest::usersAreSummarized()
{
    const QList<UserSummary> users = m_admin->users();
    QCOMPARE(users.size(), 2);
    const UserSummary &anna = users.at(0);
    QCOMPARE(anna.login, QStringLiteral("anna"));
    QCOMPARE(anna.displayName, QStringLiteral("Анна"));
    // The deleted event is a tombstone and is not counted.
    QCOMPARE(anna.records, 4);
    QCOMPARE(anna.calendars, 1);
    QCOMPARE(anna.events, 1);
    QCOMPARE(anna.tasks, 1);
    QCOMPARE(anna.devices, 2);
    QVERIFY(anna.bytes > 0);
    QVERIFY(anna.lastSeen.isValid());
    QVERIFY(!anna.lastSync.isValid());   // no exchange was counted

    // Boris got the copies of the shared calendar under his twin.
    const UserSummary &boris = users.at(1);
    QCOMPARE(boris.devices, 1);
    QCOMPARE(boris.calendars, 1);
    QCOMPARE(boris.events, 1);
    QCOMPARE(boris.tasks, 1);

    const QList<DeviceSession> devices = m_admin->devices(m_anna.id);
    QCOMPARE(devices.size(), 2);
}

void AdminTest::calendarsShowSharesAndTwins()
{
    const QList<CalendarSummary> anna = m_admin->calendars(m_anna.id, m_admin->records(m_anna.id));
    QCOMPARE(anna.size(), 1);
    QCOMPARE(anna.first().name, QStringLiteral("Работа"));
    QCOMPARE(anna.first().role, CalendarSummary::Role::Open);
    QVERIFY(anna.first().enabled);
    QCOMPARE(anna.first().mode, ShareMode::Read);
    QCOMPARE(anna.first().members.size(), 1);
    QCOMPARE(anna.first().members.first().login, QStringLiteral("boris"));
    QCOMPARE(anna.first().events, 1);
    QCOMPARE(anna.first().tasks, 1);

    const QList<CalendarSummary> boris = m_admin->calendars(m_boris.id, m_admin->records(m_boris.id));
    QCOMPARE(boris.size(), 1);
    const CalendarSummary &twin = boris.first();
    QCOMPARE(twin.id, QStringLiteral("t-work"));
    QCOMPARE(twin.name, QStringLiteral("Работа Анны"));
    QCOMPARE(twin.role, CalendarSummary::Role::Twin);
    QCOMPARE(twin.ownerLogin, QStringLiteral("anna"));
    QCOMPARE(twin.originName, QStringLiteral("Работа"));
    QCOMPARE(twin.access, ShareAccess::Read);
    QCOMPARE(twin.events, 1);
    QCOMPARE(twin.tasks, 1);
}

void AdminTest::recordsCarryTheirJson()
{
    const QList<StoredRecord> live = m_admin->records(m_anna.id);
    QCOMPARE(live.size(), 4);
    const QList<StoredRecord> all = m_admin->records(m_anna.id, true);
    QCOMPARE(all.size(), 5);
    bool sawTombstone = false;
    for (const StoredRecord &record : all) {
        if (record.id == QLatin1String("e2")) {
            QVERIFY(record.deleted);
            QVERIFY(record.data.isEmpty());
            sawTombstone = true;
        }
        if (record.id == QLatin1String("e1")) {
            QCOMPARE(record.data.value(QStringLiteral("title")).toString(), QStringLiteral("Планёрка"));
            QCOMPARE(record.device, QStringLiteral("Телефон"));
            QVERIFY(record.size > 0);
        }
    }
    QVERIFY(sawTombstone);
}

void AdminTest::totalsCountActiveAccounts()
{
    const QString hash = Api::hashPassword(QStringLiteral("secret1"), 1000);
    const UserInfo vera = m_storage->createUser(QStringLiteral("vera"), QString(), hash);
    const UserInfo gleb = m_storage->createUser(QStringLiteral("gleb"), QString(), hash);
    activity(m_anna.id, 0, 3);
    activity(m_boris.id, 5, 2);
    activity(vera.id, 20, 1);
    activity(gleb.id, 40, 4);

    const ServerTotals totals = m_admin->totals();
    QVERIFY(totals.tracked);
    QCOMPARE(totals.accounts, 4);
    QCOMPARE(totals.newAccounts, 4);
    QCOMPARE(totals.activeDay, 1);
    QCOMPARE(totals.activeWeek, 2);
    QCOMPARE(totals.activeMonth, 3);
    QCOMPARE(totals.syncsDay, 3);
    QCOMPARE(totals.syncsWeek, 5);
    QCOMPARE(totals.syncsMonth, 6);
    QCOMPARE(totals.devices, 3);
    QCOMPARE(totals.records, 4 + 3);   // Anna's four; Boris's twin and the copies of e1 and t1
    QCOMPARE(totals.tombstones, 1);
    QCOMPARE(totals.sharedCalendars, 1);
    QCOMPARE(totals.shareMembers, 1);
    // The link is spent by the API after a join; joined here directly, it still lets in.
    QCOMPARE(totals.liveLinks, 1);
    QCOMPARE(totals.trackedSince, QDate::currentDate().addDays(-40));
}

void AdminTest::historyCountsWindows()
{
    const QString hash = Api::hashPassword(QStringLiteral("secret1"), 1000);
    const UserInfo vera = m_storage->createUser(QStringLiteral("vera"), QString(), hash);
    activity(m_anna.id, 0, 3);
    activity(m_boris.id, 5, 2);
    activity(vera.id, 20, 1);

    const QDate today = QDate::currentDate();
    const QList<DayActivity> history = m_admin->history(30, today);
    QCOMPARE(history.size(), 30);
    QCOMPARE(history.first().day, today.addDays(-29));
    const DayActivity &last = history.last();
    QCOMPARE(last.day, today);
    QCOMPARE(last.daily, 1);
    QCOMPARE(last.weekly, 2);
    QCOMPARE(last.monthly, 3);
    QCOMPARE(last.syncs, 3);
    QCOMPARE(last.accounts, 3);
    QCOMPARE(last.newAccounts, 3);

    const DayActivity &fiveAgo = history.at(29 - 5);
    QCOMPARE(fiveAgo.day, today.addDays(-5));
    QCOMPARE(fiveAgo.daily, 1);
    QCOMPARE(fiveAgo.weekly, 1);
    QCOMPARE(fiveAgo.monthly, 2);
    QCOMPARE(fiveAgo.syncs, 2);
}

void AdminTest::passwordChangeSignsOut()
{
    QString error;
    QVERIFY(!m_admin->setPassword(m_anna.id, QStringLiteral("123"), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(m_storage->tokenCount(m_anna.id), 2);

    QVERIFY2(m_admin->setPassword(m_anna.id, QStringLiteral("newsecret"), &error), qPrintable(error));
    QVERIFY(Api::verifyPassword(QStringLiteral("newsecret"), m_storage->passwordHash(m_anna.id)));
    QVERIFY(!Api::verifyPassword(QStringLiteral("secret1"), m_storage->passwordHash(m_anna.id)));
    QCOMPARE(m_storage->tokenCount(m_anna.id), 0);
    QCOMPARE(m_storage->tokenCount(m_boris.id), 1);
}

void AdminTest::backupAndRestoreKeepRevisions()
{
    const QString folder = m_dir->filePath(QStringLiteral("backups"));
    QString copy;
    QString error;
    QVERIFY2(Backups::create(path(), folder, &copy, &error), qPrintable(error));
    const QList<BackupFile> list = Backups::list(folder);
    QCOMPARE(list.size(), 1);
    QVERIFY(list.first().info.ok);
    QCOMPARE(list.first().info.users, 2);

    // After the copy: one more account and more changes of Anna.
    QVERIFY(m_storage->createUser(QStringLiteral("late"), QString(), QStringLiteral("hash")).isValid());
    QJsonObject later;
    later.insert(QStringLiteral("title"), QStringLiteral("Позже"));
    QVERIFY(m_storage->sync(m_anna.id, 0, { record(QStringLiteral("event"), QStringLiteral("e3"), 3000, later) },
                            QStringLiteral("Телефон"))
                .ok);
    const qint64 revision = m_storage->userById(m_anna.id).revision;

    // Nobody may hold the base open during a restore.
    m_admin->close();
    m_storage->close();
    {
        QSqlDatabase db = QSqlDatabase::database(QStringLiteral("activity"), false);
        db.close();
    }

    const RestoreOutcome outcome = Backups::restore(copy, path(), folder);
    QVERIFY2(outcome.ok, qPrintable(outcome.error));
    QCOMPARE(outcome.users, 2);
    QVERIFY(QFileInfo::exists(outcome.safetyCopy));
    QCOMPARE(Storage::inspect(outcome.safetyCopy).users, 3);

    QVERIFY(m_storage->open(path(), &error));
    QVERIFY(!m_storage->userByLogin(QStringLiteral("late")).isValid());
    QCOMPARE(m_storage->recordData(m_anna.id, QStringLiteral("event"), QStringLiteral("e3")), QByteArray());
    // The counter stays where the devices saw it last.
    QCOMPARE(m_storage->userById(m_anna.id).revision, revision);

    // A copy that is not a base is refused, and the base is left alone.
    const QString junk = m_dir->filePath(QStringLiteral("junk.db"));
    QFile file(junk);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("not a database at all");
    file.close();
    m_storage->close();
    QVERIFY(!Backups::restore(junk, path(), folder).ok);
    QCOMPARE(Storage::inspect(path()).users, 2);
}

void AdminTest::serviceCommandLineNamesTheBase()
{
    const QString ini = m_dir->filePath(QStringLiteral("service/server.ini"));
    QDir().mkpath(QFileInfo(ini).absolutePath());
    QFile file(ini);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("[server]\nport=8799\ndata_dir=.\nlog_file=server.log\nallow_registration=false\n");
    file.close();

    ServerConfig config;
    QString error;
    QVERIFY2(ServiceControl::configOf({ QStringLiteral("--service"), QStringLiteral("--config"), ini },
                                      &config, &error),
             qPrintable(error));
    const QString folder = QFileInfo(ini).absolutePath();
    QCOMPARE(config.databasePath(), QDir(folder).filePath(QStringLiteral("server.db")));
    QCOMPARE(config.resolvedLogFile(true), QDir(folder).filePath(QStringLiteral("server.log")));
    QCOMPARE(config.port, quint16(8799));
    QVERIFY(!config.allowRegistration);
    QCOMPARE(config.configFile, QFileInfo(ini).absoluteFilePath());

    // The service writes server.log in its data folder even without log_file.
    ServerConfig plain;
    QVERIFY(ServiceControl::configOf({ QStringLiteral("--data-dir"), folder }, &plain, &error));
    QCOMPARE(plain.resolvedLogFile(true), QDir(folder).filePath(QStringLiteral("server.log")));
    QCOMPARE(plain.resolvedLogFile(false), QString());

    QVERIFY(!ServiceControl::configOf({ QStringLiteral("--no-such-option") }, &plain, &error));
}

void AdminTest::plannerRecordsAreReadForDisplay()
{
    QJsonObject event;
    event.insert(QStringLiteral("title"), QStringLiteral("Отпуск"));
    event.insert(QStringLiteral("start"), QStringLiteral("2026-10-26T00:00:00"));
    event.insert(QStringLiteral("allDay"), true);
    event.insert(QStringLiteral("cancelled"), true);
    QCOMPARE(PlannerRecord::title(QStringLiteral("event"), event), QStringLiteral("Отпуск"));
    QCOMPARE(PlannerRecord::momentText(QStringLiteral("event"), event), QStringLiteral("26.10.2026"));
    QCOMPARE(PlannerRecord::state(QStringLiteral("event"), event), QStringLiteral("отменено"));

    QJsonObject task;
    task.insert(QStringLiteral("title"), QStringLiteral("Отчёт"));
    task.insert(QStringLiteral("dueAt"), QStringLiteral("2026-10-27T18:30:00"));
    task.insert(QStringLiteral("hasDueTime"), true);
    task.insert(QStringLiteral("done"), true);
    QCOMPARE(PlannerRecord::momentText(QStringLiteral("task"), task), QStringLiteral("27.10.2026 18:30"));
    QCOMPARE(PlannerRecord::state(QStringLiteral("task"), task), QStringLiteral("выполнена"));

    QJsonObject calendar;
    calendar.insert(QStringLiteral("name"), QStringLiteral("Дом"));
    QCOMPARE(PlannerRecord::title(QStringLiteral("calendar"), calendar), QStringLiteral("Дом"));
    QCOMPARE(PlannerRecord::kindName(QStringLiteral("habitmark")), QStringLiteral("Отметка привычки"));
    QCOMPARE(PlannerRecord::kindName(QStringLiteral("future")), QStringLiteral("future"));
    QVERIFY(PlannerRecord::momentText(QStringLiteral("melody"), QJsonObject()).isEmpty());
}

void AdminTest::missingServiceIsReported()
{
#ifdef Q_OS_WIN
    const ServiceControl service(QStringLiteral("CosmosCalendarServer-no-such-%1")
                                     .arg(QUuid::createUuid().toString(QUuid::Id128)));
    QCOMPARE(service.status().state, ServiceControl::State::Missing);
#else
    QSKIP("systemd is asked only on a machine with the unit");
#endif
}

QTEST_GUILESS_MAIN(AdminTest)
#include "tst_admin.moc"
