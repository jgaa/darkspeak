#include "desktop_notifications.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QVariantMap>

#include "ds/dsengine.h"
#include "ds/file.h"
#include "ds/identity.h"
#include "ds/message.h"

namespace ds::app {

namespace {

constexpr auto service = "org.freedesktop.Notifications";
constexpr auto objectPath = "/org/freedesktop/Notifications";
constexpr auto interfaceName = "org.freedesktop.Notifications";
constexpr auto applicationName = "DarkSpeak";
constexpr auto applicationIcon = "org.darkspeak.DarkSpeak";
constexpr auto fallbackSound = "message-new-instant";

QDBusInterface notificationInterface()
{
    return {service, objectPath, interfaceName, QDBusConnection::sessionBus()};
}

}

DesktopNotifications::DesktopNotifications(QObject *parent)
    : QObject(parent)
{
    queryCapabilities();

    auto &engine = core::DsEngine::instance();

    connect(engine.getMessageManager(), &core::MessageManager::messageAdded,
            this, [this](const core::Message::ptr_t& message) {
        try {
            if (!message || message->getDirection() != core::Message::INCOMING) {
                return;
            }
            const auto conversation = message->getConversation();
            const auto title = conversation && conversation->getFirstParticipant()
                    ? conversation->getFirstParticipant()->getName()
                    : tr("Incoming message");
            showNotification(NotificationType::IncomingMessage,
                             title, message->getContent());
        } catch (...) {
            // Desktop notifications are optional and must not affect delivery.
        }
    });

    connect(engine.getFileManager(), &core::FileManager::fileAdded,
            this, [this](const core::File::ptr_t& file) {
        try {
            if (!file || file->getDirection() != core::File::INCOMING) {
                return;
            }
            const auto conversation = file->getConversation();
            const auto sender = conversation && conversation->getFirstParticipant()
                    ? conversation->getFirstParticipant()->getName()
                    : tr("Incoming file");
            showNotification(NotificationType::IncomingFile,
                             sender, tr("Incoming file: %1").arg(file->getName()));
        } catch (...) {
            // Desktop notifications are optional and must not affect transfers.
        }
    });

    connect(engine.getIdentityManager(), &core::IdentityManager::newContactRequest,
            this, [this](core::Identity *identity, const core::PeerAddmeReq& request) {
        try {
            const auto identityName = identity ? identity->getName() : tr("DarkSpeak");
            const auto title = tr("New contact request for %1").arg(identityName);
            const auto body = request.nickName.isEmpty()
                    ? tr("A new contact request has arrived.")
                    : tr("%1 wants to become a contact.").arg(request.nickName);
            showNotification(NotificationType::IncomingContactRequest, title, body);
        } catch (...) {
            // Desktop notifications are optional and must not affect requests.
        }
    });
}

void DesktopNotifications::queryCapabilities()
{
    if (capabilitiesQueried_) {
        return;
    }
    capabilitiesQueried_ = true;

    const auto reply = notificationInterface().call(QStringLiteral("GetCapabilities"));
    if (reply.type() == QDBusMessage::ReplyMessage
            && !reply.arguments().isEmpty()) {
        capabilities_ = reply.arguments().constFirst().toStringList();
    }
}

void DesktopNotifications::showNotification(const NotificationType type,
                                             const QString& title,
                                             const QString& body)
{
    const auto category = type == NotificationType::IncomingMessage
            ? QStringLiteral("im.received")
            : type == NotificationType::IncomingFile
                ? QStringLiteral("transfer")
                : QStringLiteral("im");

    QVariantMap hints;
    hints.insert(QStringLiteral("category"), category);
    hints.insert(QStringLiteral("urgency"), QVariant::fromValue<quint8>(1));
    if (capabilities_.contains(QStringLiteral("sound"))) {
        hints.insert(QStringLiteral("sound-name"), QString::fromLatin1(fallbackSound));
    }

    auto interface = notificationInterface();
    if (!interface.isValid()) {
        return;
    }

    interface.call(QStringLiteral("Notify"),
                   QString::fromLatin1(applicationName),
                   quint32{0},
                   QString::fromLatin1(applicationIcon),
                   title,
                   body,
                   QStringList{},
                   hints,
                   qint32{-1});
}

} // namespace ds::app
