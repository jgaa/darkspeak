
#include <memory>
#include <algorithm>

#include "ds/conversationmanager.h"
#include "ds/dsengine.h"
#include "ds/identity.h"

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

    if (!conversation) {
        conversation = Conversation::load(*this, uuid);
        registry_.add(uuid, conversation);
    }

    touch(conversation);
    return conversation;
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
    return conversation;
}

void ConversationManager::touch(const Conversation::ptr_t &conversation)
{
    lru_cache_.touch(conversation);
    conversationTouched(conversation);
}



}}
