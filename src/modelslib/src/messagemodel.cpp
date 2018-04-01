#include "include/ds/messagemodel.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QStringLiteral>

namespace ds {
namespace models {

MessageModel::MessageModel(QSettings &settings)
    : settings_{settings}
{
    select();
}

void MessageModel::onMessageCreated(const core::Message &message)
{
    QString sql = "INSERT INTO message (direction, conversation, contact, message_id, "
            "composed_time, sent_time, received_time, content, signature, from, encoding) "
                  "VALUES (:direction, :conversation, :contact, :message_id, "
            ":composed_time, :sent_time, :received_time, :content, :signature, :from, :encoding)";

    QSqlQuery query;
    query.prepare(sql);

    query.bindValue(":direction", static_cast<int>(message.direction));
    query.bindValue(":conversation", message.conversation);
    query.bindValue(":contact", message.contact);
    query.bindValue(":message_id", message.message_id);
    query.bindValue(":composed_time", message.composed_time.toTime_t());
    query.bindValue(":sent_time", message.sent_time.toTime_t());
    query.bindValue(":received_time", message.received_time.toTime_t());
    query.bindValue(":content", message.content);
    query.bindValue(":signature", message.signature);
    query.bindValue(":from", message.from);
    query.bindValue(":encoding", static_cast<int>(message.encoding));

    if (!query.exec()) {
        qWarning() << "Failed to add message. error:  " << query.lastError();
    }
}

void MessageModel::sendMessage(const QString &content, const QByteArray &conversation)
{
    // TODO: Create a message-request and send it to the core
}

void MessageModel::onMessageSent(const int id)
{
    // TODO: Optimize. Only realod if we are viewing this conversation

    Q_UNUSED(id);

    select();
}

void MessageModel::select()
{
    const auto sql = QStringLiteral(R"(select id, direction, conversation, contact, message_id, composed_time, sent_time, received_time, content, signature, `from`, encoding from message where conversation = %1 order by id)")
            .arg(conversation_);

    this->setQuery(sql);
}


}} // namespaces
