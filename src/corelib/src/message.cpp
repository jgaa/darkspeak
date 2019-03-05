
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

void Message::setSentReceivedTime(const QDateTime when)
{
    if (updateIf("received_time", when, sentReceivedTime_, this, &Message::onReceivedChanged)) {
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
        QString::number(data_->composedTime.currentSecsSinceEpoch()).toUtf8(),
        data_->content.toUtf8(),
        data_->sender,
        QString::number(static_cast<int>(data_->encoding)).toUtf8()});
}

bool Message::validate(const DsCert &cert) const
{
    return  cert.verify(data_->signature,
        {data_->conversation,
            data_->messageId,
            QString::number(data_->composedTime.currentSecsSinceEpoch()).toUtf8(),
            data_->content.toUtf8(),
            data_->sender,
            QString::number(static_cast<int>(data_->encoding)).toUtf8()
                        });
}

void Message::addToDb()
{
    QSqlQuery query;

    query.prepare("INSERT INTO message ("
                  "id, direction, conversation_id, conversation, message_id, composed_time, received_time, content, signature, sender, encoding"
                  ") VALUES ("
                  ":id, :direction, :conversation_id, :conversation, :message_id, :composed_time, :received_time, :content, :signature, :sender, :encoding"
                  ")");
    assert(data_);
    assert(data_->composedTime.isValid());

    query.bindValue(":id", id_);
    query.bindValue(":direction", static_cast<int>(direction_));
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
        throw Error(QStringLiteral("Failed to add Contact: %1").arg(
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
        direction, conversation_id, conversation, message_id, composed_time, received_time, content, signature, sender, encoding
    };

    query.prepare("SELECT direction, conversation_id, conversation, message_id, composed_time, received_time, content, signature, sender, encoding FROM message where id=:id ");
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
    ptr->conversationId_ = query.value(conversation_id).toInt();
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
