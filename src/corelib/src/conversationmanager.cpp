
#include <memory>
#include <algorithm>

#include "ds/conversationmanager.h"
#include "ds/dsengine.h"
#include "ds/identity.h"
#include "ds/database.h"

#include "logfault/logfault.h"

namespace ds {
namespace core {

using namespace std;

ConversationManager::ConversationManager(QObject &parent)
: QObject{&parent}
{

}

Conversation::ptr_t ConversationManager::getConversation(const QUuid &uuid)
{
    auto conversation = registry_.fetch(uuid);
    if (conversation) {
        assert(conversation->getUuid() == uuid);
    }

    if (!conversation) {
        conversation = Conversation::load(*this, uuid);
        assert(conversation->getUuid() == uuid);
        registry_.add(conversation->getUuid(), conversation);
        initConnections(conversation);
    }

    touch(conversation);
    assert(conversation->getUuid() == uuid);
    return conversation;
}

Conversation::ptr_t ConversationManager::getConversation(const int dbId)
{
    QSqlQuery query;
    query.prepare("SELECT uuid FROM conversation WHERE id=:id");
    query.bindValue(":id", dbId);
    query.exec();
    if (query.next()) {
        return getConversation(query.value(0).toUuid());
    }

    return {};
}

Conversation::ptr_t ConversationManager::getConversation(Contact *participant)
{
    QSqlQuery query;
    query.prepare("SELECT uuid FROM conversation WHERE participants=:uuid AND identity=:identity");
    query.bindValue(":uuid", participant->getUuid());
    query.bindValue(":identity", participant->getIdentityId());
    query.exec();
    if (query.next()) {
        return getConversation(query.value(0).toUuid());
    }

    return addConversation({}, {}, participant);
}

Conversation::ptr_t ConversationManager::getConversation(const QByteArray &hash, Contact *participant)
{
    QSqlQuery query;
    query.prepare("SELECT uuid FROM conversation WHERE hash=:hash AND participants=:uuid AND identity=:identity");
    query.bindValue(":hash", hash);
    query.bindValue(":uuid", participant->getUuid());
    query.bindValue(":identity", participant->getIdentityId());
    query.exec();
    if (query.next()) {
        return getConversation(query.value(0).toUuid());
    }

    return {};
}

void ConversationManager::deleteConversation(const QUuid &uuid)
{
    try {
        auto conversation = getConversation(uuid);

        // Remove from model(s)
        emit conversationDeleted(uuid);

        lru_cache_.remove(conversation);
        registry_.remove(uuid);

        conversation->deleteFromDb();
    } catch (const NotFoundError&) {
        // It's OK
    }

    emit conversationDeleted(uuid);
}

Conversation::ptr_t ConversationManager::addConversation(const QString &name, const QString &topic, Contact *participant)
{
    auto conversation = make_shared<Conversation>(*this, name, topic, participant);
    conversation->addToDb();

    registry_.add(conversation->getUuid(), conversation);
    touch(conversation);
    emit conversationAdded(conversation);
    initConnections(conversation);
    return conversation;
}

void ConversationManager::touch(const Conversation::ptr_t &conversation)
{
    lru_cache_.touch(conversation);
}

void ConversationManager::initConnections(const Conversation::ptr_t& conversation)
{
    auto conversationPtr = conversation.get();
    connect(conversation.get(), &Conversation::lastActivityChanged,
            [this, conversationPtr]() {

        LFLOG_TRACE << "Emitting conversationTouched for conversation #" << conversationPtr->getId();
        emit conversationTouched(conversationPtr->shared_from_this());
    });
}

}}
