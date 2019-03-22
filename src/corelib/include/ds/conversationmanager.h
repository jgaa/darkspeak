#ifndef CONVERSATIONMANAGER_H
#define CONVERSATIONMANAGER_H

#include <deque>
#include <QUuid>
#include <QObject>

#include "ds/conversation.h"
#include "ds/identity.h"
#include "ds/registry.h"
#include "ds/lru_cache.h"


namespace ds {
namespace core {

class ConversationManager : public QObject
{
    Q_OBJECT
public:
    ConversationManager(QObject& parent);

    // Get existing conversation
    Conversation::ptr_t getConversation(const QUuid& uuid);

    // Get existing conversation
    Conversation::ptr_t getConversation(const int dbId);

    // Get or create a new p2p conversation with this contact
    Conversation::ptr_t getConversation(Contact *participant);

    // Get ane
    Conversation::ptr_t getConversation(const QByteArray& hash, Contact *participant);

    // Delete a conversations and all its messages
    void deleteConversation(const QUuid& uuid);

    // Add p2p conversation
    Conversation::ptr_t addConversation(const QString& name, const QString& topic, Contact *participant);

    // Put the Conversation at the front of the lru cache
    void touch(const Conversation::ptr_t& conversation);

signals:
    void conversationAdded(const Conversation::ptr_t& conversation);
    void conversationDeleted(const QUuid& conversation);
    void conversationTouched(const Conversation::ptr_t& conversation);

private:
    Registry<QUuid, Conversation> registry_;
    LruCache<Conversation::ptr_t> lru_cache_{3};

};

}} // namespaces

#endif // CONVERSATIONMANAGER_H
