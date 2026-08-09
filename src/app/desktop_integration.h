#pragma once

#include <QObject>

class DesktopIntegration final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool darkSpeakFolderForced READ darkSpeakFolderForced CONSTANT)

public:
    explicit DesktopIntegration(QObject *parent = nullptr);

    [[nodiscard]] bool darkSpeakFolderForced() const;
    Q_INVOKABLE bool initializeDarkSpeakFolder();
    Q_INVOKABLE bool setDarkSpeakFolderEnabled(bool enabled);
    Q_INVOKABLE bool initializeAutostart(bool enabled);
    Q_INVOKABLE bool setAutostartEnabled(bool enabled);

private:
    bool updateDarkSpeakFolder(bool forcePaths) const;
    bool updateAutostart(bool enabled, QString *errorMessage) const;
};
