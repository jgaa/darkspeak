#include <iostream>

#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QStandardPaths>
#include <QQmlContext>
#include <QQuickStyle>

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

#include "logfault/logfault.h"

using namespace std;
using namespace ds::models;
using namespace ds::core;
using namespace ds::prot;
using namespace logfault;

int main(int argc, char *argv[])
{
    //QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);

#ifdef QT_DEBUG
    LogManager::Instance().AddHandler(
                make_unique<StreamHandler>(
                    clog, LogLevel::DEBUGGING));
#endif

    LFLOG_DEBUG << "Darkspeak is starting up cwd='"
                << QDir::currentPath()
                << '\'';

    ProtocolManager::addFactory(
                ProtocolManager::Transport::TOR,
                [](QSettings& settings) {
        return make_shared<TorProtocolManager>(settings);
    });


    QGuiApplication app(argc, argv);

    QGuiApplication::setOrganizationName("JgaasInternet");
    QGuiApplication::setOrganizationDomain("jgaa.com");

    // Initialize crypto
    ds::crypto::Crypto crypto;

#ifdef QT_DEBUG
    QGuiApplication::setApplicationName("DarkSpeak-debug");
#else
    QGuiApplication::setApplicationName("DarkSpeak");
#endif

    auto manager = make_unique<Manager>();
    manager->connect(&app, &QGuiApplication::aboutToQuit, manager.get(), &Manager::onQuit);

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

    LFLOG_INFO << "==================================================================";
    LFLOG_INFO << "==================================================================";
    LFLOG_INFO << "==================================================================";
    LFLOG_INFO << "DarkSpeak is ready";

    const auto rval = QGuiApplication::exec();

    LFLOG_NOTICE << "Darkspeak is done. So Long, and Thanks for All the Fish.";

    return rval;
}
