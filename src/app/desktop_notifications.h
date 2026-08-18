#pragma once

#include <QObject>
#include <QStringList>

namespace ds::app {

enum class NotificationType {
    IncomingMessage,
    IncomingFile,
    IncomingContactRequest
};

class DesktopNotifications final : public QObject
{
    Q_OBJECT

public:
    explicit DesktopNotifications(QObject *parent = nullptr);

    void showNotification(NotificationType type,
                          const QString& title,
                          const QString& body);

private:
    void queryCapabilities();

    bool capabilitiesQueried_ = false;
    QStringList capabilities_;
};

} // namespace ds::app
