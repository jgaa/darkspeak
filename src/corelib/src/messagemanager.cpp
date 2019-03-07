#include "ds/messagemanager.h"
#include "ds/dsengine.h"

#include "logfault/logfault.h"

namespace ds {
namespace core {

using namespace std;

MessageManager::MessageManager(QObject &parent)
: QObject{&parent}
{
}

Message::ptr_t MessageManager::getMessage(int dbId)
{
    auto message = registry_.fetch(dbId);

    if (!message) {
        message = Message::load(*this, dbId);
        registry_.add(dbId, message);
    }

    touch(message);
    return message;
}

Message::ptr_t MessageManager::getMessage(const QByteArray &messageId)
{
    QSqlQuery query;
    query.prepare("SELECT id FROM message WHERE message_id=:mid");
    query.bindValue(":mid", messageId);
    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to fetch Message from hash: %1").arg(
                        query.lastError().text()));
    }

    if (query.next()) {
        return getMessage(query.value(0).toInt());
    }

    return {};
}

Message::ptr_t MessageManager::sendMessage(Conversation &conversation, MessageData data)
{
    auto message = make_shared<Message>(*this, data, Message::OUTGOING, conversation.getId());

    assert(conversation.getIdentity());

    auto cert = conversation.getIdentity()->getCert();
    message->sign(*cert);
    message->addToDb();
    registry_.add(message->getId(), message);
    touch(message);

    emit messageAdded(message);

    if (auto contact = conversation.getFirstParticipant()) {
        contact->queueMessage(message);
    }

    return message;
}

Message::ptr_t MessageManager::receivedMessage(Conversation &conversation, MessageData data)
{
    auto message = make_shared<Message>(*this, data, Message::INCOMING, conversation.getId());

    assert(conversation.getIdentity());

    auto cert = conversation.getIdentity()->getCert();
    if (!message->validate(*cert)) {
        LFLOG_WARN << "Incoming message from " << conversation.getFirstParticipant()->getName()
                   << " to " << conversation.getIdentity()->getName()
                   << " failed validation. Rejecting.";
        return {};
    }

    // See if we already have received this message
    if (auto existing = getMessage(data.messageId)) {
        return existing;
    }

    message->addToDb();
    registry_.add(message->getId(), message);
    touch(message);

    message->touchSentReceivedTime();

    emit messageAdded(message);
    return message;
}

void MessageManager::touch(const Message::ptr_t &message)
{
    lru_cache_.touch(message);
}

void MessageManager::onMessageReceivedDateChanged(const Message::ptr_t &message)
{
    touch(message);
    emit messageReceivedDateChanged(message);
}


}} // Namespaces

