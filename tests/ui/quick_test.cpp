#include <QtQuickTest/quicktest.h>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QQuickWindow>
#include <QQuickItem>
#include <QFileInfo>
#include <QDir>
#include "appearance.h"
#include "controls_style.h"
#include "mock_ui_model.h"
#include "desktop_integration.h"
#include "settings_session.h"

class UiSetup final : public QObject {
    Q_OBJECT
public slots:
    bool captureWindow(QObject *object, const QString &path) {
        auto *window = qobject_cast<QQuickWindow *>(object);
        if (auto *item = qobject_cast<QQuickItem *>(object)) window = item->window();
        const auto capture_directory = qEnvironmentVariable("DARKSPEAK_UI_CAPTURE_DIR", directory_.path());
        return window && QDir().mkpath(capture_directory)
            && window->grabWindow().save(QDir(capture_directory).filePath(QFileInfo(path).fileName()));
    }
    void populate(bool value) { backend_.populate(value ? &entity_ : nullptr); }
    void applicationAvailable() {
        ds::ui::configureControls();
        QCoreApplication::setOrganizationName("DarkSpeakUiTest");
        QCoreApplication::setApplicationName("fixtures");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory_.path());
        qmlRegisterUncreatableType<MockNavigation>("org.darkspeak", 1, 0, "Manager", "Mock singleton");
        for (const auto *name : {"Identity", "Contact", "Conversation", "File", "Message", "MessagesModel", "NotificationsModel"})
            qmlRegisterUncreatableType<MockEntity>("org.darkspeak", 1, 0, name, "Mock type");
        qmlRegisterType<MockEntity>("org.darkspeak", 1, 0, "QmlIdentityReq");
        qmlRegisterType<MockIdentityValidator>("org.darkspeak", 1, 0, "IdentityNameValidator");
        backend_.message_type = MockEntity::MESSAGE;
        appearance_ = new Appearance(this);
        settings_ = new SettingsSession(*appearance_, desktop_, this);
    }
    void qmlEngineAvailable(QQmlEngine *engine) {
        engine->rootContext()->setContextProperty("appearance", appearance_);
        engine->rootContext()->setContextProperty("uiTest", this);
        engine->rootContext()->setContextProperty("mockMessages", &messages_);
        engine->rootContext()->setContextProperty("manager", &navigation_);
        engine->rootContext()->setContextProperty("settingsSession", settings_);
        engine->rootContext()->setContextProperty("desktopIntegration", &desktop_);
        engine->rootContext()->setContextProperty("mainWindow", &window_);
        for (const auto *name : {"identities", "contacts", "notifications", "conversations", "messages", "files", "log", "filteredLog"})
            engine->rootContext()->setContextProperty(name, &backend_);
    }
private:
    QTemporaryDir directory_;
    Appearance *appearance_ = nullptr;
    MockUiModel messages_;
    MockNavigation navigation_;
    MockWindow window_;
    MockBackendModel backend_;
    MockEntity entity_;
    DesktopIntegration desktop_;
    SettingsSession *settings_ = nullptr;
};
QUICK_TEST_MAIN_WITH_SETUP(darkspeak_ui, UiSetup)
#include "quick_test.moc"
