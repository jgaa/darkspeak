#ifndef PEERCONNECTION_H
#define PEERCONNECTION_H

#include <memory>

#include <QUuid>
#include <QObject>

#include "ds/dscert.h"
#include "message.h"

namespace ds{
namespace core {

class PeerConnection;
class Message;
class File;

struct PeerReq
{
    PeerReq() = default;
    PeerReq(const PeerReq&) = default;
    PeerReq(std::shared_ptr<PeerConnection> peerVal, QUuid connectionIdVal, quint64 requestIdVal)
        : peer{peerVal}, connectionId{std::move(connectionIdVal)}
        , requestId{requestIdVal} {}

    std::shared_ptr<PeerConnection> peer = nullptr;
    QUuid connectionId;
    quint64 requestId;
};

struct PeerAddmeReq : public PeerReq
{
    PeerAddmeReq() = default;
    PeerAddmeReq(const PeerAddmeReq&) = default;
    PeerAddmeReq(std::shared_ptr<PeerConnection> peerVal, QUuid connectionIdVal, quint64 requestIdVal,
                 QString nickNameVal, QString messageVal, QByteArray addressVal,
                 QByteArray handleVal)
        : PeerReq{peerVal, std::move(connectionIdVal), requestIdVal}
        , nickName{std::move(nickNameVal)}
        , message{std::move(messageVal)}
        , address{std::move(addressVal)}
        , handle{std::move(handleVal)} {}

    QString nickName;
    QString message;
    QByteArray address;
    QByteArray handle;
};

struct PeerAck : public PeerReq
{
    PeerAck() = default;
    PeerAck(const PeerAck&) = default;

    PeerAck(std::shared_ptr<PeerConnection> peerVal, QUuid connectionIdVal, quint64 requestIdVal,
            QByteArray whatVal, QByteArray statusVal, QString dataVal = {})
        : PeerReq{peerVal, std::move(connectionIdVal), requestIdVal}
        , what{std::move(whatVal)}, status{std::move(statusVal)}
        , data{std::move(dataVal)} {}

    QByteArray what;
    QByteArray status;
    QString data;
};

struct PeerMessage : public PeerReq
{
    PeerMessage(std::shared_ptr<PeerConnection> peer, QUuid connectionId, quint64 requestId,
                QByteArray conversation,
                QByteArray messageId,
                QDateTime composedTime,
                QString content,
                QByteArray sender,
                Message::Encoding encoding,
                QByteArray signature)
    : PeerReq{peer, std::move(connectionId), requestId}
    {
        data.conversation = std::move(conversation);
        data.messageId = std::move(messageId);
        data.composedTime = composedTime;
        data.content = std::move(content);
        data.sender = std::move(sender);
        data.encoding = encoding;
        data.signature = std::move(signature);
    }

    MessageData data;
};

struct PeerFileOffer : public PeerReq
{
    PeerFileOffer(const PeerFileOffer&) = default;

    PeerFileOffer(std::shared_ptr<PeerConnection> peer, QUuid connectionId, quint64 requestId,
                QByteArray conversation,
                QByteArray fileId,
                QString name,
                qlonglong size,
                qlonglong rest,
                QString type,
                QByteArray sha512)
    : PeerReq{peer, std::move(connectionId), requestId}
    {
        this->conversation = std::move(conversation);
        this->fileId = std::move(fileId);
        this->name = std::move(name);
        this->size = size;
        this->rest = rest;
        this->type = type;
        this->sha512 = std::move(sha512);
    }

    QByteArray conversation;
    QByteArray fileId;
    QString name;
    qlonglong size;
    qlonglong rest;
    QString type;
    QByteArray sha512;
};

struct PeerSendFile : public PeerReq
{
    PeerSendFile(const PeerSendFile&) = default;

    PeerSendFile(std::shared_ptr<PeerConnection> peer, QUuid connectionId, quint64 requestId,
                 QByteArray fileId,
                 qlonglong rest,
                 int channel)
    : PeerReq{peer, std::move(connectionId), requestId}
    {
        this->fileId = std::move(fileId);
        this->rest = rest;
        this->channel = channel;
    }

    QByteArray fileId;
    qlonglong rest = {};
    int channel = {};
};


/*! Representation of a incoming or outgoing connection to a contact.
 *
 *  Owned by the protocol module. Lifetime is purely managed as std::shared_ptr.
 */

class PeerConnection
        : public QObject
        , public std::enable_shared_from_this<PeerConnection>
{
    Q_OBJECT

public:
    using ptr_t = std::shared_ptr<PeerConnection>;

    PeerConnection() = default;

    enum Direction { INCOMING, OUTGOING };

    virtual QUuid getConnectionId() const = 0;
    virtual void authorize(bool allow) = 0;
    virtual Direction getDirection() const noexcept = 0;
    virtual crypto::DsCert::ptr_t getPeerCert() const noexcept = 0;
    virtual QUuid getIdentityId() const noexcept = 0;
    virtual void close() = 0;
    virtual uint64_t sendAck(const QString& what, const QString& status, const QString& data = {}) = 0;
    virtual bool isConnected() const noexcept = 0;
    virtual uint64_t sendMessage(const Message& message) = 0;
    virtual uint64_t offerFile(const File& file) = 0;
    virtual uint64_t startTransfer(const File& file) = 0;

signals:
    void connectedToPeer(const std::shared_ptr<PeerConnection>& peer);
    void disconnectedFromPeer(const std::shared_ptr<PeerConnection>& peer);
    void receivedData(const quint32 channel, const quint64 id, const QByteArray& data);
    void addmeRequest(const PeerAddmeReq& req);
    void receivedAck(const PeerAck& ack);
    void receivedMessage(const PeerMessage& msg);
    void receivedFileOffer(const PeerFileOffer& msg);
    void outputBufferEmptied();
};

}}

Q_DECLARE_METATYPE(ds::core::PeerConnection *)
//Q_DECLARE_SMART_POINTER_METATYPE(std::shared_ptr)
//Q_DECLARE_METATYPE(ds::core::PeerConnection::ptr_t)
Q_DECLARE_METATYPE(ds::core::PeerReq)
Q_DECLARE_METATYPE(ds::core::PeerAddmeReq)
Q_DECLARE_METATYPE(ds::core::PeerAck)

#endif // PEERCONNECTION_H
