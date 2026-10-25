#include "adminsession.h"

#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTimer>

#include <memory>

AdminSession::AdminSession(const QStringList &arguments, const QString &serviceName,
                           bool viaService, QObject *parent)
    : QObject(parent)
    , m_arguments(arguments)
    , m_viaService(viaService)
    , m_service(serviceName)
    , m_network(new QNetworkAccessManager(this))
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
    m_db.close();
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
    const bool changed = status.state != m_status.state || status.pid != m_status.pid
                         || status.exitCode != m_status.exitCode || status.error != m_status.error;
    m_status = status;
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
    QNetworkRequest request(QUrl(QStringLiteral("http://%1:%2/api/ping").arg(host).arg(m_config.port)));
    request.setTransferTimeout(3000);
    auto timer = std::make_shared<QElapsedTimer>();
    timer->start();
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, timer] {
        PingAnswer answer;
        answer.at = QDateTime::currentDateTime();
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() == QNetworkReply::NoError
            && body.value(QStringLiteral("service")).toString()
                   == QLatin1String("calendar-planner-server")) {
            answer.answered = true;
            answer.milliseconds = int(timer->elapsed());
            answer.version = body.value(QStringLiteral("version")).toString();
            answer.api = body.value(QStringLiteral("api")).toInt();
            answer.registration = body.value(QStringLiteral("registration")).toBool(true);
        } else {
            // The text of Qt is English; the usual cases are said here.
            switch (reply->error()) {
            case QNetworkReply::NoError:
                answer.error = QStringLiteral("на порту отвечает не сервер синхронизации");
                break;
            case QNetworkReply::ConnectionRefusedError:
                answer.error = QStringLiteral("порт никто не слушает");
                break;
            case QNetworkReply::OperationCanceledError:
            case QNetworkReply::TimeoutError:
                answer.error = QStringLiteral("нет ответа за 3 секунды");
                break;
            case QNetworkReply::RemoteHostClosedError:
                answer.error = QStringLiteral("соединение закрыто, не дождавшись ответа");
                break;
            default:
                answer.error = reply->errorString();
                break;
            }
        }
        reply->deleteLater();
        m_pinging = false;
        m_ping = answer;
        emit pingChanged();
    });
}
