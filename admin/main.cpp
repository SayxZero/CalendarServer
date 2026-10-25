#include "adminsession.h"
#include "look.h"
#include "mainwindow.h"
#include "servicecontrol.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QTimer>

namespace {

const char *kTitle = "Админ-панель сервера";

void complain(const QString &text)
{
    QMessageBox::warning(nullptr, QString::fromUtf8(kTitle), text);
}

} // namespace

/*!
 * The admin panel of the sync server. Without arguments it administers the
 * installed service: the settings come off the service's own command line,
 * and with them the base. --config or --data-dir point it at a server of
 * another folder instead (a test one, a copy) - then the service is only shown.
 */
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("CalendarPlanner"));
    QApplication::setApplicationName(QStringLiteral("CalendarServerAdmin"));
    QApplication::setApplicationVersion(QStringLiteral(SERVER_VERSION));
    Look::apply(app);

    QCommandLineParser parser;
    parser.setApplicationDescription(QString::fromUtf8(kTitle));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption configOption(
        { QStringLiteral("c"), QStringLiteral("config") },
        QStringLiteral("INI-файл сервера: панель работает с его базой, а не со службой."),
        QStringLiteral("file"));
    const QCommandLineOption dataOption(
        { QStringLiteral("d"), QStringLiteral("data-dir") },
        QStringLiteral("Папка сервера с server.db: панель работает с ней, а не со службой."),
        QStringLiteral("path"));
    const QCommandLineOption serviceOption(
        QStringLiteral("service-name"),
        QStringLiteral("Имя службы сервера (по умолчанию %1).").arg(ServiceControl::defaultName()),
        QStringLiteral("name"));
    QCommandLineOption grabOption(QStringLiteral("grab"),
                                  QStringLiteral("Снять все страницы в PNG в эту папку и выйти."),
                                  QStringLiteral("folder"));
    grabOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOptions({ configOption, dataOption, serviceOption, grabOption });
    // A window program has no console for process() to print into.
    if (!parser.parse(QApplication::arguments())) {
        complain(parser.errorText());
        return 2;
    }
    if (parser.isSet(QStringLiteral("help"))) {
        QMessageBox::information(nullptr, QString::fromUtf8(kTitle), parser.helpText());
        return 0;
    }
    if (parser.isSet(QStringLiteral("version"))) {
        QMessageBox::information(nullptr, QString::fromUtf8(kTitle),
                                 QStringLiteral("CalendarServerAdmin %1").arg(QStringLiteral(SERVER_VERSION)));
        return 0;
    }

    const QString serviceName = parser.isSet(serviceOption) ? parser.value(serviceOption)
                                                            : ServiceControl::defaultName();
    QStringList serverArguments;
    bool viaService = false;
    if (parser.isSet(configOption) || parser.isSet(dataOption)) {
        if (parser.isSet(configOption))
            serverArguments << QStringLiteral("--config") << parser.value(configOption);
        if (parser.isSet(dataOption))
            serverArguments << QStringLiteral("--data-dir") << parser.value(dataOption);
    } else {
        // The service, when there is one: its command line names the base.
        // Without it the panel opens the folder a server in a console uses.
        const ServiceControl service(serviceName);
        const ServiceControl::State state = service.status().state;
        if (state != ServiceControl::State::Missing && state != ServiceControl::State::Unknown) {
            QString error;
            serverArguments = service.arguments(&error);
            if (!error.isEmpty()) {
                complain(QStringLiteral("Не удалось прочитать, как запускается служба %1: %2")
                             .arg(serviceName, error));
                return 1;
            }
            viaService = true;
        }
    }

    // The base of the service lies in a folder only administrators may read,
    // and only they may start and stop the service. Its server.ini lies there
    // too: read without the rights, it looks missing - so the rights come
    // first and the settings after.
    if (viaService && !ServiceControl::isElevated()) {
        bool declined = false;
        if (ServiceControl::relaunchElevated(QApplication::arguments().mid(1), &declined))
            return 0;
        complain(declined ? QStringLiteral("Админ-панели нужны права администратора: база сервера и "
                                           "его служба доступны только им.")
                          : QStringLiteral("Не удалось запустить админ-панель с правами "
                                           "администратора. Запустите её от имени администратора."));
        return 1;
    }

    ServerConfig config;
    QString problem;
    if (!ServiceControl::configOf(serverArguments, &config, &problem)) {
        complain(QStringLiteral("Настройки сервера не читаются: %1").arg(problem));
        return 1;
    }

    AdminSession session(serverArguments, serviceName, viaService);
    MainWindow window(&session);

    if (parser.isSet(grabOption)) {
        // Rendered but never put on the screen: nobody's desktop is touched.
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 860);
        window.show();
        const QString folder = parser.value(grabOption);
        QTimer::singleShot(2500, &window, [&window, folder] {
            window.grabPages(folder);
            QApplication::quit();
        });
        return app.exec();
    }

    window.show();
    return app.exec();
}
