#ifndef MESSAGEMODEL_H
#define MESSAGEMODEL_H

#include <QSettings>
#include <QSqlQueryModel>

#include "ds/identity.h"
#include "ds/message.h"

namespace ds {
namespace models {

class MessageModel : public QSqlQueryModel
{
public:
    enum Headers {
        H_ID,
        H_DIRECTION,
        H_CONVERSATION,
        H_CONTACT,
        H_COMPOSED_TIME,
        H_SENT_TIME,
        H_RECEIVED_TIME,
        H_CONTENT,
        H_SIGNATURE,
        H_FROM,
        H_ENCODING
    };

    MessageModel(QSettings& settings);

public slots:

    /*! Signal that a message is created.
     *
     * It's now the responsibility of the MessageModel to store
     * the message to the database. Once stored, it will be sent
     * by the delivery framework as soon as possible.
     */
    void onMessagePrepared(const ds::core::Message& message);

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

    /*! Re-query the table */
    void select();

private:
    QSettings& settings_;
    QString conversation_;
};

}} // namespaces

#endif // MESSAGEMODEL_H
