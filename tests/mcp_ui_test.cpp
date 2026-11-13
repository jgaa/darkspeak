#include "appearance.h"
#include "desktop_integration.h"
#include "settings_session.h"
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>
class McpUiFixture final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList pending READ pending NOTIFY changed)
    Q_PROPERTY(QVariantList activity READ activity NOTIFY changed)
    Q_PROPERTY(QVariantList keys READ empty NOTIFY changed)
    Q_PROPERTY(QVariantList rules READ empty NOTIFY changed)
    Q_PROPERTY(QVariantList identities READ empty NOTIFY changed)
    Q_PROPERTY(QVariantList contactPolicies READ empty NOTIFY changed)
    Q_PROPERTY(QStringList capabilities READ capabilities CONSTANT)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
  public:
    QString chosen, scope;
    QVariantList pending() const {
        return {QVariantMap{{"id", "operation"},
                            {"key", "Fixture key"},
                            {"identity", "Fixture identity"},
                            {"tool", "message_send"},
                            {"contacts", QStringList{"a", "b"}},
                            {"arguments", "{\"text\":\"<b>literal private fixture</b>\"}"},
                            {"remaining", 30}}};
    }
    QVariantList activity() const {
        return {QVariantMap{{"key", "Fixture key"},
                            {"tool", "message_send"},
                            {"identity", "Fixture identity"},
                            {"state", "Pending"}}};
    }
    QVariantList empty() const {
        return {};
    }
    QStringList capabilities() const {
        return {"read_only", "send_messages"};
    }
    QString error() const {
        return {};
    }
    bool active() const {
        return true;
    }
    Q_INVOKABLE bool decide(const QString& id, const QString& decision, const QString& selected_scope) {
        if (id != "operation")
            return false;
        chosen = decision;
        scope = selected_scope;
        return true;
    }
  signals:
    void changed();
};
class McpUiTest final : public QObject {
    Q_OBJECT
  private slots:
    void actual_components_present_plaintext_and_dispatch_decision() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QCoreApplication::setOrganizationName("DarkSpeakMcpUiTest");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        Appearance appearance;
        DesktopIntegration desktop;
        SettingsSession settings(appearance, desktop);
        settings.begin();
        McpUiFixture fixture;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("appearance", &appearance);
        engine.rootContext()->setContextProperty("mcp", &fixture);
        engine.rootContext()->setContextProperty("settingsSession", &settings);
        QObject main_window;
        main_window.setProperty("width", 1000);
        main_window.setProperty("height", 700);
        engine.rootContext()->setContextProperty("mainWindow", &main_window);
        QQuickWindow window;
        window.resize(1000, 700);
        window.show();
        for (const auto* file : {"McpApprovals.qml", "McpActivity.qml", "McpSettings.qml"}) {
            QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(MCP_UI_SOURCE) + "/" + file));
            QVERIFY2(component.isReady(), qPrintable(component.errorString()));
            std::unique_ptr<QObject> object(component.create());
            QVERIFY2(object, qPrintable(component.errorString()));
            if (auto* item = qobject_cast<QQuickItem*>(object.get())) {
                item->setParentItem(window.contentItem());
                item->setWidth(900);
                item->setHeight(600);
            }
            QCoreApplication::processEvents();
            if (QString(file) == "McpApprovals.qml") {
                bool literal = false, button = false;
                // Repeater delegates are visual children; QObject ownership
                // alone does not enumerate the rendered approval cards.
                QList<QObject*> children{object.get()};
                QSet<QObject*> seen;
                for (qsizetype i = 0; i < children.size(); ++i) {
                    auto* parent = children[i];
                    if (seen.contains(parent))
                        continue;
                    seen.insert(parent);
                    for (auto* child : parent->children())
                        if (!seen.contains(child))
                            children << child;
                    if (auto* item = qobject_cast<QQuickItem*>(parent))
                        for (auto* child : item->childItems())
                            if (!seen.contains(child))
                                children << child;
                }
                for (auto* child : seen) {
                    if (child->property("text").toString().contains("literal private fixture")) {
                        QCOMPARE(child->property("textFormat").toInt(), 0);
                        literal = true;
                    }
                    if (child->property("text").toString() == "Allow once" &&
                        child->metaObject()->indexOfSignal("clicked()") >= 0) {
                        QVERIFY(QMetaObject::invokeMethod(child, "clicked"));
                        button = true;
                    }
                }
                QVERIFY(literal);
                QVERIFY(button);
                QCOMPARE(fixture.chosen, "Allow once");
                QCOMPARE(fixture.scope, "Selected contacts");
            }
        }
    }
    void activity_icon_popup_stays_inside_window() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        Appearance appearance;
        McpUiFixture fixture;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("appearance", &appearance);
        engine.rootContext()->setContextProperty("mcp", &fixture);
        QQuickWindow window;
        window.resize(640, 480);
        window.show();
        QQmlComponent component(&engine,
                                QUrl::fromLocalFile(QStringLiteral(MCP_UI_SOURCE) + "/McpActivity.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto* button = qobject_cast<QQuickItem*>(object.get());
        QVERIFY(button);
        button->setParentItem(window.contentItem());
        auto* popup = object->findChild<QObject*>("mcpActivityPopup");
        QVERIFY(popup);
        QCOMPARE(button->property("display").toInt(), 0); // AbstractButton.IconOnly
        QVERIFY(!QQmlProperty(button, "icon.source").read().toUrl().isEmpty());
        const auto inside = [popup, &window] {
            auto* parent = popup->property("parent").value<QQuickItem*>();
            if (!parent)
                return false;
            const auto point = parent->mapToItem(
                window.contentItem(), QPointF(popup->property("x").toReal(), popup->property("y").toReal()));
            return point.x() >= 0 && point.y() >= 0 &&
                   point.x() + popup->property("width").toReal() <= window.width() + 0.5 &&
                   point.y() + popup->property("height").toReal() <= window.height() + 0.5;
        };
        for (const auto scale : {100, 200}) {
            appearance.setScale(scale);
            button->setSize(QSizeF(button->implicitWidth(), button->implicitHeight()));
            button->setPosition(
                QPointF(window.width() - button->width() - 6, window.height() - button->height() - 6));
            QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
            QTRY_VERIFY(popup->property("opened").toBool());
            QTRY_VERIFY(popup->property("width").toReal() > 0);
            QVERIFY2(inside(), qPrintable(QStringLiteral("Popup %1,%2 %3x%4; window %5x%6")
                                              .arg(popup->property("x").toReal())
                                              .arg(popup->property("y").toReal())
                                              .arg(popup->property("width").toReal())
                                              .arg(popup->property("height").toReal())
                                              .arg(window.width())
                                              .arg(window.height())));
            // Resizing while open must keep the overlay within the new viewport.
            window.resize(400, 300);
            QTRY_VERIFY(inside());
            QVERIFY(QMetaObject::invokeMethod(popup, "close"));
            QTRY_VERIFY(!popup->property("visible").toBool());
            window.resize(640, 480);
        }
    }
    void failed_reconfiguration_restores_saved_settings() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        QSettings before;
        before.setValue("mcp/enabled", false);
        before.setValue("mcp/port", 17839);
        before.sync();
        Appearance appearance;
        DesktopIntegration desktop;
        SettingsSession settings(appearance, desktop);
        int calls = 0;
        settings.setMcpConfigurator([&](bool enabled, const QString&, int port, int) {
            ++calls;
            return !enabled && port == 17839;
        });
        settings.begin();
        settings.values()->insert("mcp/enabled", true);
        settings.values()->insert("mcp/port", 17840);
        QVERIFY(!settings.save());
        QCOMPARE(calls, 1);
        QVERIFY(!QSettings{}.value("mcp/enabled").toBool());
        QCOMPARE(QSettings{}.value("mcp/port").toInt(), 17839);
    }
};
QTEST_MAIN(McpUiTest)
#include "mcp_ui_test.moc"
