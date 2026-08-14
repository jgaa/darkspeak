
#include <QTime>
#include <QDate>
#include <QTimeZone>
#include <QtEndian>

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

namespace {

bool validMessageTime(const QDateTime& time)
{
    const auto isUtc = time.timeSpec() == Qt::UTC
            || (time.timeSpec() == Qt::TimeZone
                && time.timeZone().id() == QByteArrayLiteral("UTC"));
    if (!time.isValid() || !isUtc) {
        return false;
    }
    const auto minimum = QDateTime{QDate{1, 1, 1}, QTime{0, 0}, QTimeZone::UTC};
    const auto maximum = QDateTime{QDate{9999, 12, 31}, QTime{23, 59}, QTimeZone::UTC};
    const auto seconds = time.toSecsSinceEpoch();
    return seconds >= minimum.toSecsSinceEpoch()
            && seconds <= maximum.toSecsSinceEpoch()
            && seconds % 60 == 0;
}

bool validMessageContent(const MessageData& data)
{
    const auto content = data.content.toUtf8();
    if (content.size() > 65'536) {
        return false;
    }
    if (data.encoding == Message::UTF8) {
        return true;
    }
    if (data.encoding != Message::US_ACSII) {
        return false;
    }
    for (const auto byte : content) {
        const auto value = static_cast<unsigned char>(byte);
        if (!((value >= 0x20 && value <= 0x7e) || value == 0x09 || value == 0x0a)) {
            return false;
        }
    }
    return true;
}

void appendU32(QByteArray& bytes, const quint32 value)
{
    const auto encoded = qToBigEndian(value);
    bytes.append(reinterpret_cast<const char *>(&encoded), sizeof(encoded));
}

void appendU64(QByteArray& bytes, const quint64 value)
{
    const auto encoded = qToBigEndian(value);
    bytes.append(reinterpret_cast<const char *>(&encoded), sizeof(encoded));
}

QByteArray messageSignatureBytes(const MessageData& data)
{
    const auto content = data.content.toUtf8();
    QByteArray bytes = QByteArrayLiteral("DarkSpeak message signature v3");
    bytes.append(char{1}); // conversation_id source type
    appendU32(bytes, static_cast<quint32>(data.conversation.size()));
    bytes += data.conversation;
    appendU32(bytes, static_cast<quint32>(data.messageId.size()));
    bytes += data.messageId;
    appendU64(bytes, static_cast<quint64>(data.composedTime.toUTC().toSecsSinceEpoch() / 60));
    bytes += data.sender;
    appendU32(bytes, static_cast<quint32>(data.encoding == Message::UTF8 ? 2 : 1));
    appendU32(bytes, static_cast<quint32>(content.size()));
    bytes += content;
    appendU32(bytes, 0); // attachment count; attachments are not in the current UI model
    return bytes;
}

}

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
    const auto random = crypto::Crypto::getRandomBytes(32);
    data_->messageId = crypto::Crypto::getSha256(
        std::string_view{"DarkSpeak message id v3"}, data_->sender, random);

    // Don't expose the exact time the mesage was composed
    data_->composedTime = DsEngine::getSafeNow();
}

void Message::sign(const DsCert &cert)
{
    if (!data_ || data_->messageId.isEmpty() || !data_->composedTime.isValid()) {
        throw Error("Cannot sign an incomplete message");
    }

    data_->sender = cert.getSigningPubKey().toByteArray();
    data_->signature = cert.sign<QByteArray>({messageSignatureBytes(*data_)});
}

bool Message::validate(const DsCert &cert) const
{
    return data_->sender == cert.getSigningPubKey().toByteArray()
            && data_->conversation.size() == 32
            && data_->messageId.size() == 32
            && data_->signature.size() == crypto_sign_BYTES
            && validMessageTime(data_->composedTime)
            && validMessageContent(*data_)
            && cert.verify(data_->signature, {messageSignatureBytes(*data_)});
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
