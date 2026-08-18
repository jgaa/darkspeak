#include <iostream>
#include <memory>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QApplication>
#include <QMessageBox>
#include <QIcon>
#include <QLockFile>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QStandardPaths>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSize>
#include <QSettings>
#include <QWindow>

#include "ds/manager.h"
#include "ds/logmodel.h"
#include "ds/crypto.h"
#include "ds/identity.h"
#include "ds/contact.h"
#include "ds/conversation.h"
#include "ds/conversationsmodel.h"
#include "ds/messagesmodel.h"
#include "ds/message.h"
#include "ds/filesmodel.h"
#include "ds/imageprovider.h"
#include "ds/identitynamevalidator.h"
#include "ds/torprotocolmanager.h"
#include "ds/logutil.h"
#include "ds/avatarutil.h"
#include "filedialogmodel.h"
#include "desktop_integration.h"
#include "tray_controller.h"
#ifdef Q_OS_LINUX
#include "desktop_notifications.h"
#endif

#include "logfault/logfault.h"

using namespace std;
using namespace ds::models;
using namespace ds::core;
using namespace ds::prot;
using namespace logfault;

namespace {

bool earlyProfileArgument(int argc, char *argv[], QString &profile, QString &error)
{
    QStringView custom_profile;
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument == QStringLiteral("--profile")) {
            if (i + 1 == argc) {
                error = QStringLiteral("--profile requires a directory");
                return false;
            }
            profile = QString::fromLocal8Bit(argv[++i]);
        } else if (argument.startsWith(QStringLiteral("--profile="))) {
            profile = argument.mid(QStringLiteral("--profile=").size());
        }
    }

    if (profile.isEmpty()) {
        return true;
    }

    profile = QDir(profile).absolutePath();
    if (!QDir().mkpath(profile)) {
        error = QStringLiteral("Unable to create profile directory: ") + profile;
        return false;
    }
    QFile::setPermissions(profile,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner
                          | QFileDevice::ExeOwner);

    const auto setProfilePath = [&profile](const QString &name, const char *variable) {
        qputenv(variable, QDir(profile).filePath(name).toUtf8());
    };
    setProfilePath(QStringLiteral("config"), "XDG_CONFIG_HOME");
    setProfilePath(QStringLiteral("data"), "XDG_DATA_HOME");
    setProfilePath(QStringLiteral("cache"), "XDG_CACHE_HOME");
    qputenv("DARKSPEAK_PROFILE_ROOT", profile.toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QDir(profile).filePath(QStringLiteral("config")));
    return true;
}

LogLevel logLevelFromName(const QString &name, bool &ok)
{
    ok = true;
    if (name == QStringLiteral("trace")) return LogLevel::TRACE;
    if (name == QStringLiteral("debug")) return LogLevel::DEBUGGING;
    if (name == QStringLiteral("info")) return LogLevel::INFO;
    if (name == QStringLiteral("warn")) return LogLevel::WARN;
    if (name == QStringLiteral("error")) return LogLevel::ERROR;
    ok = false;
    return LogLevel::NOTICE;
}

}

int main(int argc, char *argv[])
{
    //QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);

    QString profile;
    QString earlyArgumentError;
    if (!earlyProfileArgument(argc, argv, profile, earlyArgumentError)) {
        std::cerr << earlyArgumentError.toStdString() << '\n';
        return 2;
    }

    ProtocolManager::addFactory(
                ProtocolManager::Transport::TOR,
                [](QSettings& settings) {
        return make_shared<TorProtocolManager>(settings);
    });


    QGuiApplication::setOrganizationName("DarkSpeak");
    QGuiApplication::setOrganizationDomain("darkspeak.org");
#ifdef QT_DEBUG
    QGuiApplication::setApplicationName("DarkSpeak-debug");
#else
    QGuiApplication::setApplicationName("DarkSpeak");
#endif

    QApplication app(argc, argv);

    // Desktop environments request small icon sizes repeatedly while laying
    // out and repainting the window decoration.  Keep pre-rasterized sizes so
    // the detailed SVG artwork does not get re-rasterized at subtly different
    // sub-pixel alignments on every repaint.
    QIcon applicationIcon;
    for (const auto &iconSize : {16, 22, 24, 32, 48, 64, 128, 256}) {
        applicationIcon.addFile(
            QStringLiteral(":/icons/darkspeak-%1.png").arg(iconSize),
            QSize(iconSize, iconSize));
    }
    applicationIcon.addFile(QStringLiteral(":/icons/darkspeak.svg"));
    app.setWindowIcon(applicationIcon);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("DarkSpeak"));
    const auto helpOption = parser.addHelpOption();
    const auto versionOption = parser.addVersionOption();
    parser.addOption({{QStringLiteral("p"), QStringLiteral("profile")},
                      QStringLiteral("Use an isolated profile directory."),
                      QStringLiteral("directory")});
    parser.addOption({QStringLiteral("log-file"),
                      QStringLiteral("Write logs to this file."),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("log-level"),
                      QStringLiteral("Set log level (trace, debug, info, warn, error)."),
                      QStringLiteral("level")});
    parser.addOption({QStringLiteral("qml-smoke-test"),
                      QStringLiteral("Load and validate all bundled QML files, then exit.")});
    parser.addOption({QStringLiteral("autostart"),
                      QStringLiteral("Start minimized to the taskbar.")});

    if (!parser.parse(app.arguments())) {
        std::cerr << parser.errorText().toStdString() << '\n';
        return 2;
    }
    if (parser.isSet(helpOption)) {
        parser.showHelp();
    }
    if (parser.isSet(versionOption)) {
        parser.showVersion();
    }

    const QString parsedProfile = parser.value(QStringLiteral("profile"));
    if (!parsedProfile.isEmpty() && QDir(parsedProfile).absolutePath() != profile) {
        std::cerr << "Unable to apply profile directory before Qt initialization.\n";
        return 2;
    }

    const QString requestedLogLevel = parser.value(QStringLiteral("log-level"));
    bool validLogLevel = true;
    if (!requestedLogLevel.isEmpty()) {
        logLevelFromName(requestedLogLevel, validLogLevel);
    }
    if (!validLogLevel) {
        std::cerr << "Invalid --log-level. Expected trace, debug, info, warn, or error.\n";
        return 2;
    }

    const auto lockRoot = profile.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            : profile;
    if (lockRoot.isEmpty() || !QDir().mkpath(lockRoot)) {
        std::cerr << "Unable to create the application lock directory.\n";
        return 2;
    }

    auto instanceLock = make_unique<QLockFile>(QDir(lockRoot).filePath("instance.lock"));
    if (!instanceLock->tryLock(0)) {
        QMessageBox::critical(nullptr, QObject::tr("DarkSpeak is already running"),
                              QObject::tr("Another DarkSpeak instance is already using this profile."));
        return 2;
    }

    // Initialize crypto
    ds::crypto::Crypto crypto;

    DesktopIntegration desktopIntegration(&app);
    if (!desktopIntegration.initializeDarkSpeakFolder()) {
        std::cerr << "Unable to prepare the DarkSpeak folder in the home directory.\n";
        return 2;
    }

    unique_ptr<Manager> manager;
    try {
        manager = make_unique<Manager>(parser.value(QStringLiteral("log-file")),
                                       requestedLogLevel);
    } catch (const std::exception& ex) {
        const QString detail = QString::fromUtf8(ex.what());
        std::cerr << "DarkSpeak could not start securely: "
                  << detail.toStdString() << '\n';
        QMessageBox::critical(
            nullptr,
            QObject::tr("Secure storage unavailable"),
            QObject::tr("DarkSpeak cannot start because its secure OS vault is locked or unavailable.\n\n"
                         "Unlock or restore the system vault, then start DarkSpeak again.\n"
                         "No private keys or other secrets will be stored outside the vault."));
        return 1;
    }
    manager->connect(&app, &QGuiApplication::aboutToQuit, manager.get(), &Manager::onQuit);

#ifdef Q_OS_LINUX
    [[maybe_unused]] const auto desktopNotifications =
            std::make_unique<ds::app::DesktopNotifications>(&app);
#endif

    LFLOG_DEBUG << "Darkspeak is starting up cwd='"
                << QDir::currentPath()
                << '\'';

    LFLOG_INFO << manager->getProgramNameAndVersion() << " is ready.";

    if (!parsedProfile.isEmpty()) {
        auto name = app.applicationDisplayName();
        name = name + " [" + parsedProfile + "]";

        LFLOG_DEBUG << "Changing display name to: " << name;
        app.setApplicationDisplayName(name);
    }

    qmlRegisterUncreatableType<ds::models::Manager>("org.darkspeak", 1, 0,
                                                    "Manager",
                                                    "Manager is a global sigeleton.");

    qmlRegisterUncreatableType<ds::models::LogModel>("org.darkspeak", 1, 0,
                                                     "LogModel",
                                                     "LogModel is a global sigeleton.");

    qmlRegisterUncreatableType<ds::models::ContactsModel>("org.darkspeak", 1, 0,
                                                           "ContactsModel",
                                                           "Cannot create ContactsModel in QML");

    qmlRegisterUncreatableType<ds::models::NotificationsModel>("org.darkspeak", 1, 0,
                                                           "NotificationsModel",
                                                           "Cannot create NotificationsModel in QML");

    qmlRegisterUncreatableType<ds::core::Identity>("org.darkspeak", 1, 0,
                                                   "Identity",
                                                   "Cannot create NotificationsModel in QML");

    qmlRegisterUncreatableType<ds::core::IdentityManager>("org.darkspeak", 1, 0,
                                                   "IdentityManager",
                                                   "Cannot create NotificationsModel in QML");

    qmlRegisterUncreatableType<ds::core::Contact>("org.darkspeak", 1, 0,
                                                   "Contact",
                                                   "Cannot create Contact in QML");

    qmlRegisterUncreatableType<ds::core::Conversation>("org.darkspeak", 1, 0,
                                                   "Conversation",
                                                   "Cannot create Conversation in QML");

    qmlRegisterUncreatableType<ds::models::ConversationsModel>("org.darkspeak", 1, 0,
                                                   "ConversationsModel",
                                                   "Cannot create Conversation in QML");

    qmlRegisterUncreatableType<ds::core::Message>("org.darkspeak", 1, 0,
                                                   "Message",
                                                   "Cannot create Message in QML");

    qmlRegisterUncreatableType<ds::models::MessagesModel>("org.darkspeak", 1, 0,
                                                   "MessagesModel",
                                                   "Cannot create Messages in QML");

    qmlRegisterUncreatableType<ds::core::File>("org.darkspeak", 1, 0,
                                                   "File",
                                                   "Cannot create File in QML");

    qmlRegisterUncreatableType<ds::models::FilesModel>("org.darkspeak", 1, 0,
                                                   "FilesModel",
                                                   "Cannot create FilesModel in QML");


    qmlRegisterType<ds::core::QmlIdentityReq>("org.darkspeak", 1, 0, "QmlIdentityReq");
    qmlRegisterType<ds::models::IdentityNameValidator>("org.darkspeak", 1, 0, "IdentityNameValidator");

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("desktopIntegration", &desktopIntegration);
    FileDialogModel fileDialogs;

    engine.rootContext()->setContextProperty("manager", manager.get());
    engine.rootContext()->setContextProperty("fileDialogs", &fileDialogs);
    engine.rootContext()->setContextProperty("log", manager->logModel());
    engine.rootContext()->setContextProperty("identities", DsEngine::instance().getIdentityManager());
    engine.rootContext()->setContextProperty("contacts", manager->contactsModel());
    engine.rootContext()->setContextProperty("notifications", manager->notificationsModel());
    engine.rootContext()->setContextProperty("conversations", manager->conversationsModel());
    engine.rootContext()->setContextProperty("messages", manager->messagesModel());
    engine.rootContext()->setContextProperty("files", manager->filesModel());

    auto tmpProvider = new ImageProvider{"temp", [&manager](const QString& id) {
            Q_UNUSED(id)
            return manager->getTmpImage();
        }};

    engine.addImageProvider(tmpProvider->getName(), tmpProvider);

    auto identityProvider = new ImageProvider{"identity", [](const QString& id) -> QImage {
            if (auto identity = DsEngine::instance().getIdentityManager()->identityFromUuid(QUuid{id})) {
                return circularAvatar(identity->getAvatar());
            }

            return {};
        }};

    engine.addImageProvider(identityProvider->getName(), identityProvider);

    auto contactProvider = new ImageProvider{"contact", [](const QString& id) -> QImage {
            if (auto contact = DsEngine::instance().getContactManager()->getContact(QUuid{id})) {
                return circularAvatar(contact->getAvatar());
            }

            return {};
        }};

    engine.addImageProvider(contactProvider->getName(), contactProvider);


    //QQuickStyle::setStyle("Fusion");

    engine.load(QUrl(QStringLiteral("qrc:/main.qml")));
    if (engine.rootObjects().isEmpty()) {
        LFLOG_ERROR << "No root object after loading main.qml. Aborting mission.";
        return -1;
    }

    QSettings generalSettings;
    const bool autostart = generalSettings.value(QStringLiteral("autostart"), false).toBool();
    desktopIntegration.initializeAutostart(autostart);

    auto *window = qobject_cast<QWindow *>(engine.rootObjects().constFirst());
    std::unique_ptr<TrayController> tray;
    if (window != nullptr) {
        tray = std::make_unique<TrayController>(window,
                                                manager->notificationsModel(),
                                                parser.isSet(QStringLiteral("autostart")),
                                                &app);
        app.setQuitOnLastWindowClosed(!tray->available());
    }

    if (parser.isSet(QStringLiteral("qml-smoke-test"))) {
        QDirIterator qmlFiles(QStringLiteral(":/"), {QStringLiteral("*.qml")},
                              QDir::Files, QDirIterator::Subdirectories);
        while (qmlFiles.hasNext()) {
            const QUrl url(QStringLiteral("qrc:") + qmlFiles.next().mid(1));
            QQmlComponent component(&engine, url);
            if (component.status() == QQmlComponent::Error) {
                for (const auto &error : component.errors()) {
                    LFLOG_ERROR << "QML smoke test failed for " << url.toString()
                                << ": " << error.toString();
                }
                return -1;
            }
        }
        return 0;
    }

    LFLOG_INFO << "DarkSpeak is ready";

    const auto rval = QGuiApplication::exec();

    LFLOG_NOTICE << "Darkspeak is done. So Long, and Thanks for All the Fish.";

    return rval;
}
