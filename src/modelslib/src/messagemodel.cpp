
#include "ds/messagemodel.h"
#include "ds/dsengine.h"
#include "ds/dscert.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QSqlRecord>
#include <QUuid>


using namespace std;
using namespace ds::core;
using namespace ds::crypto;

namespace ds {
namespace models {

MessageModel::MessageModel(QSettings &settings)
    : settings_{settings}
{
    setTable("message");
    setSort(fieldIndex("id"), Qt::AscendingOrder);
    //setEditStrategy(QSqlTableModel::OnFieldChange);

    // Show nothing until we get a conversation to work with
    setFilter("conversation_id = -1");

    h_id_ = fieldIndex("id");
    h_direction_ = fieldIndex("direction");
    h_conversation_id_ = fieldIndex("conversation_id");
    h_conversation_ = fieldIndex("conversation");
    h_message_id_ = fieldIndex("message_id");
    h_composed_time_ = fieldIndex("composed_time");
    h_sent_time_ = fieldIndex("sent_time");
    h_received_time_= fieldIndex("received_time");
    h_content_ = fieldIndex("content");
    h_signature_ = fieldIndex("signature");
    h_sender_ = fieldIndex("sender");
    h_encoding_ = fieldIndex("encoding");

    select();
}

void MessageModel::save(const ds::core::Message& message)
{
    QString sql = "INSERT INTO message (direction, conversation_id, conversation, message_id, "
            "composed_time, sent_time, received_time, content, signature, sender, encoding) "
                  "VALUES (:direction, :conversation_id, :conversation, :message_id, "
            ":composed_time, :sent_time, :received_time, :content, :signature, :sender, :encoding)";

    QSqlQuery query;
    query.prepare(sql);

    query.bindValue(":direction", static_cast<int>(message.direction));
    query.bindValue(":conversation_id", conversation_id_);
    query.bindValue(":conversation", message.conversation);
    query.bindValue(":message_id", message.message_id);
    query.bindValue(":composed_time", message.composed_time.toTime_t());
    query.bindValue(":sent_time", message.sent_time.toTime_t());
    query.bindValue(":received_time", message.received_time.toTime_t());
    query.bindValue(":content", message.content);
    query.bindValue(":signature", message.signature);
    query.bindValue(":sender", message.sender);
    query.bindValue(":encoding", static_cast<int>(message.encoding));

    if (!query.exec()) {
        qWarning() << "Failed to add message. error:  " << query.lastError()
                   << ". Query: " << query.lastQuery();
    }

    select();
}

void MessageModel::sendMessage(const QString &content,
                               const core::Identity& sender,
                               const QByteArray &conversation)
{
    ModelMessage msg;
    msg.init();

    // TODO: Support usascii as an alternative
    msg.content = content.toUtf8();
    msg.encoding = Encoding::UTF8;

    msg.sender = sender.getHash();
    msg.conversation = conversation;

    msg.sign(*sender.getCert());

    save(msg);

    // TODO: Connect dsengine
    emit outgoingMessage(msg);
}

void MessageModel::onMessageSent(const int id)
{
    // TODO: Optimize. Only reload if we are viewing this conversation

    Q_UNUSED(id);

    select();
}

void MessageModel::setConversation(int conversationId, const QByteArray &conversation)
{
    conversation_ = conversation;
    conversation_id_ = conversationId;

    QString filter = QString("conversation_id =") + QString::number(conversation_id_);
    setFilter(filter);
}

}} // namespaces
