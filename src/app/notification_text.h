#pragma once

#include <QCoreApplication>
#include <QString>

namespace ds::app {

enum class NotificationType { IncomingMessage, IncomingFile, IncomingContactRequest };

struct NotificationText {
    QString title;
    QString body;
};

inline NotificationText notificationText(NotificationType type, bool show_sender,
                                         bool show_content, const QString& sender,
                                         const QString& content)
{
    if (type == NotificationType::IncomingContactRequest) {
        return {QCoreApplication::translate("DesktopNotifications", "New contact request"),
            show_sender && !sender.isEmpty()
                ? QCoreApplication::translate("DesktopNotifications", "%1 wants to become a contact.").arg(sender)
                : QCoreApplication::translate("DesktopNotifications", "A new contact request has arrived.")};
    }
    const bool file = type == NotificationType::IncomingFile;
    const auto title = show_sender && !sender.isEmpty() ? sender
        : file ? QCoreApplication::translate("DesktopNotifications", "Incoming file")
               : QCoreApplication::translate("DesktopNotifications", "Incoming message");
    const auto body = show_content
        ? (file ? QCoreApplication::translate("DesktopNotifications", "Incoming file: %1").arg(content) : content)
        : file ? QCoreApplication::translate("DesktopNotifications", "Open DarkSpeak to view the file.")
               : QCoreApplication::translate("DesktopNotifications", "Open DarkSpeak to read the message.");
    return {title, body};
}

} // namespace ds::app
