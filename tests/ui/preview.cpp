#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTemporaryDir>
#include "appearance.h"
#include "controls_style.h"
#include "mock_ui_model.h"

// Interactive, disposable visual review. No application backend, Tor, or vault.
int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    ds::ui::configureControls();
    QCoreApplication::setOrganizationName("DarkSpeakUiTest");
    QCoreApplication::setApplicationName("preview");
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    Appearance appearance;
    MockUiModel messages;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("appearance", &appearance);
    engine.rootContext()->setContextProperty("mockMessages", &messages);
    engine.load(QUrl::fromLocalFile(QStringLiteral(DS_UI_SOURCE_DIR "/UiHarness.qml")));
    if (engine.rootObjects().isEmpty()) return 1;
    return application.exec();
}
