#ifndef MESSAGEMODEL_H
#define MESSAGEMODEL_H

#include <QSettings>
#include <QSqlTableModel>

#include "ds/identity.h"
#include "ds/message.h"

namespace ds {
namespace models {

class MessageModel : public QSqlTableModel
{
    Q_OBJECT
public:

    // Message, with some extra fields to reference database id's
    class ModelMessage : public ds::core::Message {
    public:
        int conversation_id = {};
        int contact_id = {};
    };

    enum Headers {
        H_ID,
        H_DIRECTION,
        H_COMPOSED_TIME,
        H_SENT_TIME,
        H_RECEIVED_TIME,
        H_CONTENT,
        H_SENDER,
        H_ENCODING
    };

    MessageModel(QSettings& settings);

signals:
    void outgoingMessage(const core::Message& msg);

public slots:

    /*! Save a message
     *
     */
    void save(const ds::core::Message& message);

    /*! Send a message
     *
     * This method is used by the UI to send a message. The message
     * is delivered to the dsengine where it is processed and
     * returned for storage trough a messageCreated signal.
     */
    void sendMessage(const QString& content,
                     const core::Identity& from,
                     const QByteArray& conversation);

    /*! Notification that an outbound message is delivered. */
    void onMessageSent(const int id);

    void setConversation(int conversationId, const QByteArray& conversation);

private:
    QSettings& settings_;
    QString conversation_;
    int conversation_id_ = {};


    int h_id_ = {};
    int h_direction_ = {};
    int h_conversation_id_ = {};
    int h_conversation_ = {};
    int h_message_id_ = {};
    int h_composed_time_ = {};
    int h_sent_time_ = {};
    int h_received_time_ = {};
    int h_content_ = {};
    int h_signature_ = {};
    int h_sender_ = {};
    int h_encoding_ = {};
};

}} // namespaces

#endif // MESSAGEMODEL_H
