#include "adminsession.h"

#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

namespace {

const int kPingTimeoutMs = 3000;

/*!
 * One GET /api/ping, timed on a thread of its own. Timed on the thread of the
 * window, it counted whatever the window was busy with meanwhile - building
 * its pages over a base of sixty thousand records took four seconds, and the
 * panel reported a server that answers in four seconds.
 */
PingAnswer pingServer(const QString &host, quint16 port)
{
    PingAnswer answer;
    QElapsedTimer timer;
    timer.start();

    QTcpSocket socket;
    socket.connectToHost(host, port);
    if (!socket.waitForConnected(kPingTimeoutMs)) {
        answer.error = socket.error() == QAbstractSocket::ConnectionRefusedError
                           ? QStringLiteral("порт никто не слушает")
                           : socket.error() == QAbstractSocket::SocketTimeoutError
                                 ? QStringLiteral("нет ответа за 3 секунды")
                                 : socket.errorString();
        return answer;
    }
    socket.write(QByteArrayLiteral("GET /api/ping HTTP/1.1\r\nHost: ") + host.toLatin1()
                 + QByteArrayLiteral("\r\nConnection: close\r\n\r\n"));
    QByteArray received;
    while (timer.elapsed() < kPingTimeoutMs) {
        if (!socket.waitForReadyRead(int(qMax<qint64>(1, kPingTimeoutMs - timer.elapsed()))))
            break;
        received += socket.readAll();
        const int headEnd = received.indexOf("\r\n\r\n");
        if (headEnd >= 0) {
            const int at = received.toLower().indexOf("content-length:");
            const int end = at >= 0 ? received.indexOf("\r\n", at) : -1;
            const int length = at >= 0 ? received.mid(at + 15, end - at - 15).trimmed().toInt() : -1;
            if (length >= 0 && received.size() >= headEnd + 4 + length)
                break;
        }
    }
    const int elapsed = int(timer.elapsed());
    received += socket.readAll();

    const int headEnd = received.indexOf("\r\n\r\n");
    if (received.isEmpty()) {
        answer.error = timer.elapsed() >= kPingTimeoutMs
                           ? QStringLiteral("нет ответа за 3 секунды")
                           : QStringLiteral("соединение закрыто, не дождавшись ответа");
        return answer;
    }
    const QJsonObject body = QJsonDocument::fromJson(headEnd >= 0 ? received.mid(headEnd + 4)
                                                                  : QByteArray())
                                 .object();
    if (!received.startsWith("HTTP/1.1 200")
        || body.value(QStringLiteral("service")).toString()
               != QLatin1String("calendar-planner-server")) {
        answer.error = QStringLiteral("на порту отвечает не сервер синхронизации");
        return answer;
    }
    answer.answered = true;
    answer.milliseconds = elapsed;
    answer.version = body.value(QStringLiteral("version")).toString();
    answer.api = body.value(QStringLiteral("api")).toInt();
    answer.registration = body.value(QStringLiteral("registration")).toBool(true);
    return answer;
}

} // namespace

AdminSession::AdminSession(const QStringList &arguments, const QString &serviceName,
                           bool viaService, QObject *parent)
    : QObject(parent)
    , m_arguments(arguments)
    , m_viaService(viaService)
    , m_service(serviceName)
    , m_statusTimer(new QTimer(this))
    , m_pingTimer(new QTimer(this))
{
    reloadConfig();
    connect(m_statusTimer, &QTimer::timeout, this, &AdminSession::refreshStatus);
    connect(m_pingTimer, &QTimer::timeout, this, &AdminSession::refreshPing);
    m_statusTimer->start(2000);
    // Every ping is a line in the server's log, so not too often: the state
    // of the service, asked every two seconds, triggers one when it changes.
    m_pingTimer->start(60000);
    refreshStatus();
    refreshPing();
}

AdminSession::~AdminSession()
{
    // A ping still on its way would outlive the session and its thread object.
    if (m_pingThread)
        m_pingThread->wait(kPingTimeoutMs + 1000);
    m_db.close();
}

quint16 AdminSession::serverPort() const
{
    return m_runtime.running && m_runtime.port ? m_runtime.port : m_config.port;
}

void AdminSession::reloadConfig()
{
    // main() has checked these arguments once; a broken INI file now keeps
    // the settings read before.
    ServerConfig config;
    if (ServiceControl::configOf(m_arguments, &config, nullptr))
        m_config = config;
}

QString AdminSession::logPath() const
{
    // The log the running server says it writes: a console server started
    // with --log-file has one the settings know nothing about.
    if (m_runtime.running && !m_runtime.logFile.isEmpty())
        return m_runtime.logFile;
    return m_config.resolvedLogFile(m_viaService);
}

QString AdminSession::backupFolder() const
{
    QSettings settings;
    const QString saved = settings.value(QStringLiteral("backups/folder")).toString();
    if (!saved.isEmpty())
        return saved;
    return QDir(m_config.resolvedDataDir()).absoluteFilePath(QStringLiteral("backups"));
}

void AdminSession::setBackupFolder(const QString &folder)
{
    QSettings settings;
    settings.setValue(QStringLiteral("backups/folder"), folder);
}

bool AdminSession::ensureDatabase(QString *error)
{
    if (m_db.isOpen())
        return true;
    if (!m_db.open(databasePath(), error))
        return false;
    emit databaseChanged();
    return true;
}

void AdminSession::releaseDatabase()
{
    m_db.close();
}

bool AdminSession::reopenDatabase(QString *error)
{
    m_db.close();
    const bool ok = m_db.open(databasePath(), error);
    emit databaseChanged();
    return ok;
}

void AdminSession::refreshStatus()
{
    const ServiceControl::Status status = m_service.status();
    const RuntimeInfo runtime = RuntimeFile::read(m_config.resolvedDataDir());
    const bool changed = status.state != m_status.state || status.pid != m_status.pid
                         || status.exitCode != m_status.exitCode || status.error != m_status.error
                         || runtime.running != m_runtime.running || runtime.pid != m_runtime.pid
                         || runtime.port != m_runtime.port;
    m_status = status;
    m_runtime = runtime;
    // The service may have just created the base on its first start.
    if (!m_db.isOpen())
        ensureDatabase();
    if (changed) {
        emit statusChanged();
        refreshPing();
    }
}

void AdminSession::refreshPing()
{
    if (m_pinging)
        return;
    m_pinging = true;

    const bool anyAddress = m_config.host == QHostAddress::Any
                            || m_config.host == QHostAddress::AnyIPv4
                            || m_config.host == QHostAddress::AnyIPv6;
    const QString host = anyAddress ? QStringLiteral("127.0.0.1") : m_config.host.toString();
    const quint16 port = serverPort();
    QThread *thread = QThread::create([this, host, port] {
        PingAnswer answer = pingServer(host, port);
        answer.at = QDateTime::currentDateTime();
        // Back on the thread of the window; dropped if the session is gone.
        QMetaObject::invokeMethod(this, [this, answer] {
            m_pinging = false;
            m_ping = answer;
            emit pingChanged();
        }, Qt::QueuedConnection);
    });
    m_pingThread = thread;
    connect(thread, &QThread::finished, this, [this, thread] {
        if (m_pingThread == thread)
            m_pingThread = nullptr;
        thread->deleteLater();
    });
    thread->start();
}
