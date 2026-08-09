#pragma once

#include <QObject>

class DesktopIntegration final : public QObject
{
    Q_OBJECT

public:
    explicit DesktopIntegration(QObject *parent = nullptr);

    Q_INVOKABLE bool initializeAutostart(bool enabled);
    Q_INVOKABLE bool setAutostartEnabled(bool enabled);

private:
    bool updateAutostart(bool enabled, QString *errorMessage) const;
};
