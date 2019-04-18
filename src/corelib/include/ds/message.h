#ifndef MESSAGE_H
#define MESSAGE_H

#include <memory>

#include <QObject>
#include <QByteArray>
#include <QDateTime>

#include "ds/dscert.h"

namespace ds {
namespace core {

struct MessageData;
class Conversation;

class Message : public QObject, public std::enable_shared_from_this<Message>
{
    Q_OBJECT
public:
    enum Encoding
    {
        US_ACSII,
        UTF8
    };

    Q_ENUM(Encoding)

    enum Direction {
        OUTGOING,
        INCOMING
    };

    Q_ENUM(Direction)

    enum State {
        MS_COMPOSED,
        MS_QUEUED,
        MS_SENT, // Waiting for acknowledgment
        MS_RECEIVED,
        MS_REJECTED
    };

    Q_ENUM(State)

    using ptr_t = std::shared_ptr<Message>;

    Message(QObject& parent);
    Message(QObject& parent, MessageData data, Direction direction, const int conversationid);

    Q_PROPERTY(int id READ getId)
    Q_PROPERTY(Direction direction READ getDirection)
    Q_PROPERTY(QDateTime composed READ getComposedTime)
    Q_PROPERTY(QDateTime received READ getSentReceivedTime NOTIFY receivedChanged)
    Q_PROPERTY(QString content READ getContent)
    Q_PROPERTY(State state READ getState NOTIFY stateChanged)

    int getId() const noexcept;
    int getConversationId() const noexcept;
    Direction getDirection() const noexcept;
    QDateTime getComposedTime() const noexcept;
    QDateTime getSentReceivedTime() const noexcept;
    void setSentReceivedTime(const QDateTime& when);
    void touchSentReceivedTime();
    QString getContent() const noexcept;
    //Direction getType() const noexcept;
    const MessageData& getData() const noexcept;
    State getState() const noexcept;
    void setState(State state);
    Conversation *getConversation() const;

    void init();
    void sign(const crypto::DsCert& cert);
    bool validate(const crypto::DsCert& cert) const;

    /*! Add the new Message to the database. */
    void addToDb();

    /*! Delete from the database */
    void deleteFromDb();

    const char *getTableName() const noexcept { return "message"; }

    static ptr_t load(QObject& parent, int dbId);

signals:
    void receivedChanged();
    void stateChanged();

private:
    int id_ = -1;
    int conversationId_ = -1;
    Direction direction_ = Direction::OUTGOING;
    State state_ = State::MS_COMPOSED;
    QDateTime sentReceivedTime_; // Depending on type
    std::unique_ptr<MessageData> data_;
};

struct MessageData {
    // Hash key for conversation
    // For p2p, it's the receivers pubkey hash.
    QByteArray conversation;
    QByteArray messageId;
    QDateTime composedTime;
    QString content;

    // Senders pubkey hash.
    QByteArray sender;
    Message::Encoding encoding = Message::Encoding::US_ACSII;
    QByteArray signature;
};

struct MessageContent {
    Message::State state;
    QString content;
    QDateTime composedTime;
    Message::Direction direction = Message::OUTGOING;
    QDateTime sentReceivedTime;
};

}} // Namespaces

Q_DECLARE_METATYPE(ds::core::Message *)
Q_DECLARE_METATYPE(ds::core::MessageData)

#endif // MESSAGE_H
