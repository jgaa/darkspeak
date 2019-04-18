
#include <QTime>

#include "ds/crypto.h"
#include "ds/message.h"
#include "ds/memoryview.h"
#include "ds/errors.h"
#include "ds/update_helper.h"
#include "ds/dsengine.h"

#include "logfault/logfault.h"

using namespace ds::crypto;
using namespace std;

namespace ds {
namespace core {

Message::Message(QObject &parent)
    : QObject(&parent)
{

}

Message::Message(QObject &parent, MessageData data, Message::Direction direction, const int conversationid)
    : QObject{&parent}, conversationId_{conversationid}, direction_{direction}
    , data_{std::make_unique<MessageData>(std::move(data))}
{
    if (direction_ == INCOMING) {
        state_ = State::MS_RECEIVED;
        sentReceivedTime_ = DsEngine::getSafeNow();
    }
}

int Message::getId() const noexcept
{
    return id_;
}

int Message::getConversationId() const noexcept
{
    return conversationId_;
}

Message::Direction Message::getDirection() const noexcept
{
    return direction_;
}

QDateTime Message::getComposedTime() const noexcept
{
    return data_->composedTime;
}

QDateTime Message::getSentReceivedTime() const noexcept
{
    return sentReceivedTime_;
}

void Message::setSentReceivedTime(const QDateTime& when)
{
    if (updateIf("received_time", when, sentReceivedTime_, this, &Message::receivedChanged)) {
        DsEngine::instance().getMessageManager()->onMessageReceivedDateChanged(shared_from_this());
    }
}

void Message::touchSentReceivedTime()
{
    setSentReceivedTime(DsEngine::getSafeNow());
}

QString Message::getContent() const noexcept
{
    return data_->content;
}

const MessageData &Message::getData() const noexcept
{
    return *data_;
}

Message::State Message::getState() const noexcept
{
    return state_;
}

void Message::setState(Message::State state)
{
    if (updateIf("state", state, state_, this, &Message::stateChanged)) {
        DsEngine::instance().getMessageManager()->onMessageStateChanged(shared_from_this());
    }
}

Conversation *Message::getConversation() const
{
    QSqlQuery query;
    query.prepare("SELECT uuid FROM conversation WHERE id=:id");
    query.bindValue(":id", getConversationId());
    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to fetch conversation from id: %1").arg(
                        query.lastError().text()));
    }

    if (query.next()) {
        return DsEngine::instance().getConversationManager()->getConversation(query.value(0).toUuid()).get();
    }

    return {};
}

void Message::init()
{
    data_->messageId = crypto::Crypto::generateId();

    // Don't expose the exact time the mesage was composed
    data_->composedTime = DsEngine::getSafeNow();
}

void Message::sign(const DsCert &cert)
{
    assert(!data_->messageId.isEmpty());
    assert(data_->composedTime.isValid());

    data_->signature = cert.sign<QByteArray>({
        data_->conversation,
        data_->messageId,
        data_->composedTime.toString(Qt::ISODate).toUtf8(),
        data_->content.toUtf8(),
        data_->sender,
        QString::number(static_cast<int>(data_->encoding)).toUtf8()});
}

bool Message::validate(const DsCert &cert) const
{
    return  cert.verify(data_->signature,
        {data_->conversation,
            data_->messageId,
            data_->composedTime.toString(Qt::ISODate).toUtf8(),
            data_->content.toUtf8(),
            data_->sender,
            QString::number(static_cast<int>(data_->encoding)).toUtf8()
                        });
}

void Message::addToDb()
{
    QSqlQuery query;

    query.prepare("INSERT INTO message ("
                  "direction, state, conversation_id, conversation, message_id, composed_time, received_time, content, signature, sender, encoding"
                  ") VALUES ("
                  ":direction, :state, :conversation_id, :conversation, :message_id, :composed_time, :received_time, :content, :signature, :sender, :encoding"
                  ")");
    assert(data_);
    assert(data_->composedTime.isValid());

    query.bindValue(":direction", static_cast<int>(direction_));
    query.bindValue(":state", static_cast<int>(state_));
    query.bindValue(":conversation_id", conversationId_);
    query.bindValue(":conversation", data_->conversation);
    query.bindValue(":message_id", data_->messageId);
    query.bindValue(":composed_time", data_->composedTime);
    query.bindValue(":received_time", sentReceivedTime_);
    query.bindValue(":content", data_->content);
    query.bindValue(":signature", data_->signature);
    query.bindValue(":sender", data_->sender);
    query.bindValue(":encoding", data_->encoding);


    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to add Message: %1").arg(
                        query.lastError().text()));
    }

    id_ = query.lastInsertId().toInt();

    LFLOG_INFO << "Added message " << data_->messageId.toHex()
               << " to the database with id " << id_;
}

void Message::deleteFromDb()
{
    if (id_ > 0) {
        QSqlQuery query;
        query.prepare("DELETE FROM message WHERE id=:id");
        query.bindValue(":id", id_);
        if(!query.exec()) {
            throw Error(QStringLiteral("SQL Failed to delete message: %1").arg(
                            query.lastError().text()));
        }
    }
}

Message::ptr_t Message::load(QObject &parent, int dbId)
{
    QSqlQuery query;

    enum Fields {
        direction, state,  conversation_id, conversation, message_id, composed_time, received_time, content, signature, sender, encoding
    };

    query.prepare("SELECT direction, state, conversation_id, conversation, message_id, composed_time, received_time, content, signature, sender, encoding FROM message where id=:id ");
    query.bindValue(":id", dbId);

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to fetch Message: %1").arg(
                        query.lastError().text()));
    }

    if (!query.next()) {
        throw NotFoundError(QStringLiteral("Message not found!"));
    }

    auto ptr = make_shared<Message>(parent);
    ptr->data_ = make_unique<MessageData>();

    ptr->id_ = dbId;
    ptr->direction_ = static_cast<Direction>(query.value(direction).toInt());
    ptr->state_ = static_cast<State>(query.value(state).toInt());
    ptr->conversationId_ = query.value(conversation_id).toInt();
    ptr->data_->conversation = query.value(conversation).toByteArray();
    ptr->data_->messageId = query.value(message_id).toByteArray();
    ptr->data_->composedTime = query.value(composed_time).toDateTime();
    ptr->sentReceivedTime_ = query.value(received_time).toDateTime();
    ptr->data_->content = query.value(content).toString();
    ptr->data_->signature = query.value(signature).toByteArray();
    ptr->data_->sender = query.value(sender).toByteArray();
    ptr->data_->encoding = static_cast<Encoding>(query.value(encoding).toInt());

    return ptr;
}

}} // namespaces
