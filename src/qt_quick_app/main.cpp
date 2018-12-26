#include <iostream>

#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QStandardPaths>

#include "ds/manager.h"

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

    auto manager = make_unique<Manager>();

    QQmlApplicationEngine engine;
    engine.load(QUrl(QStringLiteral("qrc:/main.qml")));
    if (engine.rootObjects().isEmpty()) {
        LFLOG_ERROR << "No root object after loading main.qml. Aborting mission.";
        return -1;
    }

    const auto rval = app.exec();

    LFLOG_DEBUG << "Darkspeak is done. So Long, and Thanks for All the Fish.";

    return rval;
}
