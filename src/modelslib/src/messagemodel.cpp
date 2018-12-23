#include "ds/messagemodel.h"
#include "ds/dsengine.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>

using namespace std;
using namespace ds::core;

namespace ds {
namespace models {

MessageModel::MessageModel(QSettings &settings)
    : settings_{settings}
{
    select();
}

void MessageModel::onMessagePrepared(const core::Message &message)
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

void MessageModel::sendMessage(const QString &content,
                               const core::Identity& from,
                               const QByteArray &conversation)
{
    MessageReq msg;
    // TODO: Support usascii as an alternative
    msg.content = content.toUtf8();
    msg.encoding = Encoding::UTF8;

    msg.from = from.hash;
    msg.conversation = conversation;
    DsEngine::instance().sendMessage(msg, from);
}

void MessageModel::onMessageSent(const int id)
{
    // TODO: Optimize. Only reload if we are viewing this conversation

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
