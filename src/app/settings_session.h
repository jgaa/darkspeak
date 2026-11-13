#pragma once

#include <QObject>
#include <QQmlPropertyMap>
#include <QSettings>
#include <functional>

class Appearance;
class DesktopIntegration;

// A dialog-scoped draft. No QSettings writes or desktop side effects before Save.
class SettingsSession final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QQmlPropertyMap *values READ values CONSTANT)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
public:
    SettingsSession(Appearance &appearance, DesktopIntegration &desktop, QObject *parent = nullptr);
    QQmlPropertyMap *values() { return &values_; }
    QString error() const { return error_; }
    void setEventsConfigurator(std::function<bool(bool, int, bool)> fn) { events_configurator_ = std::move(fn); }
    void setMcpConfigurator(std::function<bool(bool, const QString&, int, int)> fn) { mcp_configurator_ = std::move(fn); }
    Q_INVOKABLE void begin();
    Q_INVOKABLE bool save();
    Q_INVOKABLE void cancel();
signals:
    void errorChanged();
private:
    bool fail(const QString &error);
    std::function<bool(bool, int, bool)> events_configurator_;
    std::function<bool(bool, const QString&, int, int)> mcp_configurator_;
    Appearance &appearance_;
    DesktopIntegration &desktop_;
    QQmlPropertyMap values_;
    QVariantMap original_;
    QString error_;
};
