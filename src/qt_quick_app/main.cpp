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
#include "ds/identitiesmodel.h"

#include "logfault/logfault.h"

using namespace std;
using namespace ds::models;
using namespace ds::core;
using namespace logfault;

int main(int argc, char *argv[])
{
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);

#ifdef QT_DEBUG
    LogManager::Instance().AddHandler(
                make_unique<StreamHandler>(
                    clog, LogLevel::DEBUGGING));
#endif

    LFLOG_DEBUG << "Darkspeak is starting up cwd='"
                << QDir::currentPath()
                << '\'';

    QGuiApplication app(argc, argv);

    app.setOrganizationName("TheLastViking");
    app.setOrganizationDomain("lastviking.eu");

    // Initialize crypto
    ds::crypto::Crypto crypto;

#ifdef QT_DEBUG
    app.setApplicationName("DarkSpeak-debug");
#else
    app.setApplicationName("DarkSpeak");
#endif

    auto manager = make_unique<Manager>();

    LFLOG_INFO << manager->getProgramNameAndVersion() << " is ready.";

    qmlRegisterUncreatableType<ds::models::Manager>("com.jgaa.darkspeak", 1, 0,
                                                    "Manager",
                                                    "ContactsModel is a global sigeleton.");

    qmlRegisterUncreatableType<ds::models::LogModel>("com.jgaa.darkspeak", 1, 0,
                                                     "LogModel",
                                                     "LogModel is a global sigeleton.");

    qmlRegisterUncreatableType<ds::models::IdentitiesModel>("com.jgaa.darkspeak", 1, 0,
                                                            "IdentitiesModel",
                                                            "IdentitiesModel is a global sigeleton.");


    qmlRegisterUncreatableType<ds::models::ContactsModel>("com.jgaa.darkspeak", 1, 0,
                                                           "ContactsModel",
                                                           "Cannot create OnlineStatus in QML");

    qmlRegisterUncreatableType<ds::models::NotificationsModel>("com.jgaa.darkspeak", 1, 0,
                                                           "ContactsModel",
                                                           "Cannot create NotificationsModel in QML");

    qmlRegisterUncreatableType<ds::core::Identity>("com.jgaa.darkspeak", 1, 0,
                                                   "Identity",
                                                   "Cannot create NotificationsModel in QML");

    qmlRegisterUncreatableType<ds::core::IdentityManager>("com.jgaa.darkspeak", 1, 0,
                                                   "IdentityManager",
                                                   "Cannot create NotificationsModel in QML");

    qmlRegisterType<ds::core::QmlIdentityReq>("com.jgaa.darkspeak", 1, 0, "QmlIdentityReq");

    QQmlApplicationEngine engine;

    engine.rootContext()->setContextProperty("manager", manager.get());
    engine.rootContext()->setContextProperty("log", manager->logModel());
    engine.rootContext()->setContextProperty("identities", DsEngine::instance().getIdentityManager());
    engine.rootContext()->setContextProperty("contacts", manager->contactsModel());
    engine.rootContext()->setContextProperty("notifications", manager->notificationsModel());

    //QQuickStyle::setStyle("Fusion");

    engine.load(QUrl(QStringLiteral("qrc:/main.qml")));
    if (engine.rootObjects().isEmpty()) {
        LFLOG_ERROR << "No root object after loading main.qml. Aborting mission.";
        return -1;
    }

    const auto rval = app.exec();

    LFLOG_DEBUG << "Darkspeak is done. So Long, and Thanks for All the Fish.";

    return rval;
}
