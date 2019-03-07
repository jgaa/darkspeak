#ifndef MESSAGEMANAGER_H
#define MESSAGEMANAGER_H

#include <deque>
#include <unordered_map>
#include <QUuid>
#include <QObject>

#include "ds/conversation.h"
#include "ds/message.h"
#include "ds/registry.h"
#include "ds/lru_cache.h"

namespace ds {
namespace core {

class MessageManager : public QObject
{
    Q_OBJECT
public:
    explicit MessageManager(QObject& parent);

    Message::ptr_t getMessage(int dbId);
    Message::ptr_t getMessage(const QByteArray& messageId);
    Message::ptr_t sendMessage(Conversation& conversation, MessageData data);
    Message::ptr_t receivedMessage(Conversation& conversation, MessageData data);
    void touch(const Message::ptr_t& message);

    void onMessageReceivedDateChanged(const Message::ptr_t& message);
    void onMessageStateChanged(const Message::ptr_t& message);


signals:
    void messageAdded(const Message::ptr_t& message);
    void messageDeleted(const Message::ptr_t& message);
    void messageReceivedDateChanged(const Message::ptr_t& message);
    void messageStateChanged(const Message::ptr_t& message);

private:
    Registry<int, Message> registry_;
    LruCache<Message::ptr_t> lru_cache_{3};

};

}} // namespaces

#endif // MESSAGEMANAGER_H
