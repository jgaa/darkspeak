#include "include/ds/messagemanager.h"

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

