#include "ds/messagemanager.h"
#include "ds/dsengine.h"
#include "ds/database.h"

#include <QSqlError>

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

Message::ptr_t MessageManager::getMessage(const QByteArray &messageId,
                                          const int conversationId)
{
    QSqlQuery query;
    query.prepare("SELECT id FROM message WHERE conversation_id=:cid and message_id=:mid");
    query.bindValue(":cid", conversationId);
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

Message::ptr_t MessageManager::getMessage(const QByteArray &messageId, const Message::Direction direction)
{
    QSqlQuery query;
    query.prepare("SELECT id FROM message WHERE message_id=:mid and direction=:direction");
    query.bindValue(":mid", messageId);
    query.bindValue(":direction", static_cast<int>(direction));
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
    if (!message->validate(*cert)) {
        throw Error("Invalid v3 outgoing message");
    }
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
    assert(conversation.getFirstParticipant());

    auto cert = conversation.getFirstParticipant()->getCert();
    if (!message->validate(*cert)) {
        LFLOG_WARN << "Incoming message from " << conversation.getFirstParticipant()->getName()
                   << " to " << conversation.getIdentity()->getName()
                   << " failed validation. Rejecting.";
        return {};
    }

    // MessageId is globally scoped. Exact retransmission is idempotent, but
    // reusing an id for different signed contents is an authenticity error.
    QSqlQuery existingQuery;
    existingQuery.prepare("SELECT id FROM message WHERE message_id=:mid");
    existingQuery.bindValue(":mid", data.messageId);
    if (!existingQuery.exec()) {
        throw Error(QStringLiteral("Failed to query duplicate message: %1")
                    .arg(existingQuery.lastError().text()));
    }
    if (existingQuery.next()) {
        auto existing = getMessage(existingQuery.value(0).toInt());
        const auto& old = existing->getData();
        const auto same = old.conversation == data.conversation
                && old.messageId == data.messageId
                && old.composedTime.toUTC().toSecsSinceEpoch() / 60
                    == data.composedTime.toUTC().toSecsSinceEpoch() / 60
                && old.content == data.content
                && old.sender == data.sender
                && old.encoding == data.encoding;
        if (!same) {
            throw Error("MessageId was reused with different signed contents");
        }
        return existing;
    }

    message->addToDb();
    registry_.add(message->getId(), message);
    touch(message);

    emit messageAdded(message);
    return message;
}

bool MessageManager::hasConflictingMessage(const MessageData& data)
{
    QSqlQuery query;
    query.prepare("SELECT id FROM message WHERE message_id=:mid");
    query.bindValue(":mid", data.messageId);
    if (!query.exec() || !query.next()) {
        return query.lastError().type() != QSqlError::NoError;
    }

    const auto existing = Message::load(*this, query.value(0).toInt());
    const auto& old = existing->getData();
    return old.conversation != data.conversation
            || old.composedTime.toUTC().toSecsSinceEpoch() / 60
                != data.composedTime.toUTC().toSecsSinceEpoch() / 60
            || old.content != data.content
            || old.sender != data.sender
            || old.encoding != data.encoding;
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

void MessageManager::onMessageStateChanged(const Message::ptr_t &message)
{
    touch(message);
    LFLOG_DEBUG << "Message state changed to " << message->getState();
    emit messageStateChanged(message);
}


}} // Namespaces
