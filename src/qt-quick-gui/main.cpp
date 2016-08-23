
#include <boost/filesystem.hpp>

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QtQml>
#include <QStandardPaths>
#include <qqmlengine.h>
#include <qqmlcontext.h>

#include "log/WarLog.h"

#include "darkspeak/darkspeak.h"
#include "darkspeak-gui.h"
#include "darkspeak/ImManager.h"

#include "DarkRoot.h"
#include "ContactsModel.h"
#include "ChatMessagesModel.h"
#include "FileTransferModel.h"
#include "ContactData.h"
#include "SettingsData.h"

using namespace darkspeak;
using namespace war;
using namespace std;

#ifdef _MSC_VER
// Prevent Microsoft's "telemetry" (phone home) crap
// https://www.jgaa.com/index.php?cmd=show_article&article_id=1051
extern "C"
{
	void _cdecl __vcrt_initialize_telemetry_provider() {}
	void _cdecl __telemetry_main_invoke_trigger() {}
	void _cdecl __telemetry_main_return_trigger() {}
	void _cdecl __vcrt_uninitialize_telemetry_provider() {}
};
#endif // _MSC_VER


int main(int argc, char *argv[])
{
    // Let QT eat it's command-line arguments
    QApplication app(argc, argv);

    // Prepare our own stuff
    log::LogEngine logger;

#ifdef DEBUG
    logger.AddHandler(make_shared<war::log::LogToStream>(
        clog, "console", log::LL_DEBUG));
#endif

    LOG_DEBUG << "Starting up with cwd='"
        << boost::filesystem::current_path().string()
        << "'";

    boost::filesystem::path conf_path = QStandardPaths::locate(
                QStandardPaths::AppDataLocation,
                {},
                QStandardPaths::LocateDirectory).toUtf8().constData();

    if (conf_path.empty()) {
        conf_path = QStandardPaths::locate(
                    QStandardPaths::HomeLocation,
                    {},
                    QStandardPaths::LocateDirectory).toUtf8().constData();

        conf_path /= ".darkspeak";
    }

    if (!boost::filesystem::is_directory(conf_path)) {
        LOG_NOTICE << "Creating data-dir: " << log::Esc(conf_path.string());
        boost::filesystem::create_directory(conf_path);
    }

    boost::filesystem::current_path(conf_path);

#ifdef DEBUG
    logger.AddHandler(make_shared<war::log::LogToFile>(
        "darkspeak.log", true, "file", log::LL_TRACE4));
#else
    logger.AddHandler(make_shared<war::log::LogToFile>(
        "darkspeak.log", true, "file", log::LL_NOTICE));
#endif

    LOG_INFO << "Using "
        << log::Esc(boost::filesystem::current_path().string())
        << " as data-dir and current dir.";

    conf_path /= "darkspeak.conf";

    // TODO: Create configuration file if it don't exist.
    //      Pop up a configuration dialog when the Gui starts.

	if (!boost::filesystem::is_regular_file(conf_path)) {
		std::ofstream cf(conf_path.string().c_str());
	}

    auto manager = impl::ImManager::CreateInstance(conf_path);
    DarkRoot dark_root(*manager, manager->GetConfig());
    ContactsModel contacts_model(*manager);
    FileTransferModel file_transfer_model(*manager);

    dark_root.SetImageProvider(contacts_model.GetAvatarProvider());

    qRegisterMetaType<darkspeak::Api::Message::ptr_t>("darkspeak::Api::Message::ptr_t");
    qRegisterMetaType<std::string>("std::string");
    qRegisterMetaType<darkspeak::FileInfo>("darkspeak::FileInfo");
    qRegisterMetaType<FileTransferModel::State>("FileTransferModel::State");
    qRegisterMetaType<FileTransferModel::Direction>("FileTransferModel::Direction");
    qRegisterMetaType<QVector<int>>("QVector<int>");

    // Initiallze the UI components
    QQmlApplicationEngine engine;

    {
        QString no_create_message = "ContactsModel is a global signleton.";
        qmlRegisterUncreatableType<ContactsModel>("com.jgaa.darkspeak", 1, 0, "ContactsModel", no_create_message);
    }

    {
        QString no_create_message = "FileTransferModel is a global signleton.";
        qmlRegisterUncreatableType<FileTransferModel>("com.jgaa.darkspeak", 1, 0, "FileTransferModel", no_create_message);
    }

    {
        QString no_create_message = "ChatMessagesModel is obtained from ContactsModel.";
        qmlRegisterUncreatableType<ChatMessagesModel>("com.jgaa.darkspeak", 1, 0, "ChatMessagesModel", no_create_message);
    }


    //{
    //    QString no_create_message = "ContactData is obtained from ContactsModel.";
    //    qmlRegisterUncreatableType<ContactData>("com.jgaa.darkspeak", 1, 0, "ContactData", no_create_message);
    //}

    qmlRegisterType<ContactData>("com.jgaa.darkspeak", 1, 0, "ContactData");
    qmlRegisterType<SettingsData>("com.jgaa.darkspeak", 1, 0, "SettingsData");


    //qmlRegisterType<ContactsModel>("com.jgaa.darkspeak", 1, 0, "ContactsModel");
    //FoldersModel folders_model(*client);

    engine.rootContext()->setContextProperty("darkRoot", &dark_root);
    engine.rootContext()->setContextProperty("contactsModel", &contacts_model);
    engine.rootContext()->setContextProperty("transfersModel", &file_transfer_model);
    engine.addImageProvider("buddy", contacts_model.GetAvatarProvider()); // QT takes ownership of the instance ?
    engine.load(QUrl("qrc:/qml/main.qml"));


    auto rval = app.exec();

    manager->Disconnect();

    // TODO: Wait for the threadpool to finish
    manager.reset();

    return rval;
}


