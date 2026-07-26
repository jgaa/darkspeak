#include <iostream>
#include <memory>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLockFile>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QStandardPaths>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSettings>

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

#include "logfault/logfault.h"

using namespace std;
using namespace ds::models;
using namespace ds::core;
using namespace ds::prot;
using namespace logfault;

namespace {

bool earlyProfileArgument(int argc, char *argv[], QString &profile, QString &error)
{
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

void configureLogging(const QString &logFile, const QString &logLevel, LogModel *logModel)
{
    if (logFile.isEmpty() && logLevel.isEmpty()) {
        return;
    }

    bool valid = true;
#ifdef QT_DEBUG
    const auto defaultLevel = LogLevel::DEBUGGING;
#else
    const auto defaultLevel = LogLevel::NOTICE;
#endif
    const auto level = logLevel.isEmpty() ? defaultLevel
                                          : logLevelFromName(logLevel, valid);
    Q_ASSERT(valid);

    auto &settings = DsEngine::instance().settings();
    auto &logManager = LogManager::Instance();
    logManager.ClearHandlers();

    const auto appLevel = logLevel.isEmpty()
            ? getLogLevel(LogSystem::APPLICATION) : level;
    const auto stdoutLevel = logLevel.isEmpty()
            ? getLogLevel(LogSystem::STDOUT) : level;
    if (appLevel != LogLevel::DISABLED) {
        logModel->installHandler(appLevel);
    }
    if (stdoutLevel != LogLevel::DISABLED) {
        logManager.AddHandler(make_unique<StreamHandler>(clog, stdoutLevel));
    }

    if (!logFile.isEmpty()) {
        const QFileInfo fileInfo(logFile);
        QDir().mkpath(fileInfo.absolutePath());
        logManager.AddHandler(make_unique<StreamHandler>(
            logFile.toStdString(), logLevel.isEmpty() ? defaultLevel : level, true));
    } else if (isEnabled(LogSystem::LOGFILE)) {
        const auto path = settings.value("logPath").toString();
        logManager.AddHandler(make_unique<StreamHandler>(
            path.toStdString(), getLogLevel(LogSystem::LOGFILE), true));
    }
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


    QGuiApplication::setOrganizationName("JgaasInternet");
    QGuiApplication::setOrganizationDomain("jgaa.com");
#ifdef QT_DEBUG
    QGuiApplication::setApplicationName("DarkSpeak-debug");
#else
    QGuiApplication::setApplicationName("DarkSpeak");
#endif

    QGuiApplication app(argc, argv);

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

    unique_ptr<QLockFile> profileLock;
    if (!profile.isEmpty()) {
        profileLock = make_unique<QLockFile>(QDir(profile).filePath("instance.lock"));
        if (!profileLock->tryLock(0)) {
            std::cerr << "The profile is already in use: " << profile.toStdString() << '\n';
            return 2;
        }
    }

    // Initialize crypto
    ds::crypto::Crypto crypto;

    auto manager = make_unique<Manager>();
    manager->connect(&app, &QGuiApplication::aboutToQuit, manager.get(), &Manager::onQuit);

    configureLogging(parser.value(QStringLiteral("log-file")), requestedLogLevel, manager->logModel());
    LFLOG_DEBUG << "Darkspeak is starting up cwd='"
                << QDir::currentPath()
                << '\'';

    LFLOG_INFO << manager->getProgramNameAndVersion() << " is ready.";

    qmlRegisterUncreatableType<ds::models::Manager>("com.jgaa.darkspeak", 1, 0,
                                                    "Manager",
                                                    "Manager is a global sigeleton.");

    qmlRegisterUncreatableType<ds::models::LogModel>("com.jgaa.darkspeak", 1, 0,
                                                     "LogModel",
                                                     "LogModel is a global sigeleton.");

    qmlRegisterUncreatableType<ds::models::ContactsModel>("com.jgaa.darkspeak", 1, 0,
                                                           "ContactsModel",
                                                           "Cannot create ContactsModel in QML");

    qmlRegisterUncreatableType<ds::models::NotificationsModel>("com.jgaa.darkspeak", 1, 0,
                                                           "ContactsModel",
                                                           "Cannot create NotificationsModel in QML");

    qmlRegisterUncreatableType<ds::core::Identity>("com.jgaa.darkspeak", 1, 0,
                                                   "Identity",
                                                   "Cannot create NotificationsModel in QML");

    qmlRegisterUncreatableType<ds::core::IdentityManager>("com.jgaa.darkspeak", 1, 0,
                                                   "IdentityManager",
                                                   "Cannot create NotificationsModel in QML");

    qmlRegisterUncreatableType<ds::core::Contact>("com.jgaa.darkspeak", 1, 0,
                                                   "Contact",
                                                   "Cannot create Contact in QML");

    qmlRegisterUncreatableType<ds::core::Conversation>("com.jgaa.darkspeak", 1, 0,
                                                   "Conversation",
                                                   "Cannot create Conversation in QML");

    qmlRegisterUncreatableType<ds::models::ConversationsModel>("com.jgaa.darkspeak", 1, 0,
                                                   "ConversationsModel",
                                                   "Cannot create Conversation in QML");

    qmlRegisterUncreatableType<ds::core::Message>("com.jgaa.darkspeak", 1, 0,
                                                   "Message",
                                                   "Cannot create Message in QML");

    qmlRegisterUncreatableType<ds::models::MessagesModel>("com.jgaa.darkspeak", 1, 0,
                                                   "MessagesModel",
                                                   "Cannot create Messages in QML");

    qmlRegisterUncreatableType<ds::core::File>("com.jgaa.darkspeak", 1, 0,
                                                   "File",
                                                   "Cannot create File in QML");

    qmlRegisterUncreatableType<ds::models::FilesModel>("com.jgaa.darkspeak", 1, 0,
                                                   "FilesModel",
                                                   "Cannot create FilesModel in QML");


    qmlRegisterType<ds::core::QmlIdentityReq>("com.jgaa.darkspeak", 1, 0, "QmlIdentityReq");
    qmlRegisterType<ds::models::IdentityNameValidator>("com.jgaa.darkspeak", 1, 0, "IdentityNameValidator");

    QQmlApplicationEngine engine;

    engine.rootContext()->setContextProperty("manager", manager.get());
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
                return identity->getAvatar();
            }

            return {};
        }};

    engine.addImageProvider(identityProvider->getName(), identityProvider);

    auto contactProvider = new ImageProvider{"contact", [](const QString& id) -> QImage {
            if (auto contact = DsEngine::instance().getContactManager()->getContact(QUuid{id})) {
                return contact->getAvatar();
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

    LFLOG_INFO << "==================================================================";
    LFLOG_INFO << "==================================================================";
    LFLOG_INFO << "==================================================================";
    LFLOG_INFO << "DarkSpeak is ready";

    const auto rval = QGuiApplication::exec();

    LFLOG_NOTICE << "Darkspeak is done. So Long, and Thanks for All the Fish.";

    return rval;
}
