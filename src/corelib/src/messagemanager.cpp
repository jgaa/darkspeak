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

