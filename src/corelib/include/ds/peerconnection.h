#ifndef PEERCONNECTION_H
#define PEERCONNECTION_H

#include <memory>

#include <QUuid>
#include <QObject>
#include <QVariantMap>
#include <QImage>

#include "ds/dscert.h"
#include "ds/message.h"
#include "ds/userinfo.h"

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
                 QByteArray handleVal, QByteArray avatarHashVal = {})
        : PeerReq{peerVal, std::move(connectionIdVal), requestIdVal}
        , nickName{std::move(nickNameVal)}
        , message{std::move(messageVal)}
        , address{std::move(addressVal)}
        , handle{std::move(handleVal)}
        , avatarHash{std::move(avatarHashVal)} {}

    QString nickName;
    QString message;
    QString address;
    QString handle;
    QByteArray avatarHash;
};

struct PeerSetAvatarReq : public PeerReq
{
    PeerSetAvatarReq() = default;
    PeerSetAvatarReq(const PeerSetAvatarReq&) = default;
    PeerSetAvatarReq(std::shared_ptr<PeerConnection> peerVal, QUuid connectionIdVal, quint64 requestIdVal,
                 QImage avatar)
        : PeerReq{peerVal, std::move(connectionIdVal), requestIdVal}
        , avatar{std::move(avatar)}
        {}

    QImage avatar;
};

enum class ConnectionResponseStatus {
    Pending,
    Accepted,
    Rejected,
    Blocked
};

enum class FileOfferResponseStatus {
    Accepted,
    Rejected
};

enum class FileCancelReason {
    SourceChanged,
    UserAction
};

struct PeerConnectionResponse : public PeerReq
{
    PeerConnectionResponse() = default;

    PeerConnectionResponse(std::shared_ptr<PeerConnection> peerVal, QUuid connectionIdVal,
                           quint64 requestIdVal, ConnectionResponseStatus statusVal,
                           QString messageVal)
        : PeerReq{peerVal, std::move(connectionIdVal), requestIdVal}
        , status{statusVal}, message{std::move(messageVal)} {}

    ConnectionResponseStatus status = ConnectionResponseStatus::Rejected;
    QString message;
};

struct PeerMessageReceipt : public PeerReq
{
    PeerMessageReceipt(std::shared_ptr<PeerConnection> peerVal, QUuid connectionIdVal,
                       quint64 requestIdVal, QByteArray messageIdVal)
        : PeerReq{peerVal, std::move(connectionIdVal), requestIdVal}
        , messageId{std::move(messageIdVal)} {}

    QByteArray messageId;
};

struct PeerFileOfferResponse : public PeerReq
{
    PeerFileOfferResponse(std::shared_ptr<PeerConnection> peerVal, QUuid connectionIdVal,
                          quint64 requestIdVal, QByteArray fileIdVal,
                          FileOfferResponseStatus statusVal)
        : PeerReq{peerVal, std::move(connectionIdVal), requestIdVal}
        , fileId{std::move(fileIdVal)}, status{statusVal} {}

    QByteArray fileId;
    FileOfferResponseStatus status = FileOfferResponseStatus::Rejected;
};

struct PeerFileAck : public PeerReq
{
    PeerFileAck(std::shared_ptr<PeerConnection> peerVal, QUuid connectionIdVal,
                quint64 requestIdVal, QByteArray fileIdVal, quint64 nextSequenceVal,
                quint64 nextOffsetVal, bool finalVal)
        : PeerReq{peerVal, std::move(connectionIdVal), requestIdVal}
        , fileId{std::move(fileIdVal)}, nextSequence{nextSequenceVal}
        , nextOffset{nextOffsetVal}, final{finalVal} {}

    QByteArray fileId;
    quint64 nextSequence = 0;
    quint64 nextOffset = 0;
    bool final = false;
};

struct PeerFileCancel : public PeerReq
{
    PeerFileCancel(std::shared_ptr<PeerConnection> peerVal, QUuid connectionIdVal,
                   quint64 requestIdVal, QByteArray fileIdVal,
                   FileCancelReason reasonVal)
        : PeerReq{peerVal, std::move(connectionIdVal), requestIdVal}
        , fileId{std::move(fileIdVal)}, reason{reasonVal} {}

    QByteArray fileId;
    FileCancelReason reason = FileCancelReason::UserAction;
};

struct PeerFileResume : public PeerReq
{
    PeerFileResume(std::shared_ptr<PeerConnection> peerVal, QUuid connectionIdVal,
                   quint64 requestIdVal, QByteArray fileIdVal, quint64 nextOffsetVal,
                   QByteArray sha256Val, quint64 fileSizeVal)
        : PeerReq{peerVal, std::move(connectionIdVal), requestIdVal}
        , fileId{std::move(fileIdVal)}, nextOffset{nextOffsetVal}
        , sha256{std::move(sha256Val)}, fileSize{fileSizeVal} {}

    QByteArray fileId;
    quint64 nextOffset = 0;
    QByteArray sha256;
    quint64 fileSize = 0;
};

enum class FileResumeStatus {
    Accepted,
    Rejected,
    Cancelled
};

struct PeerFileResumeResult : public PeerReq
{
    PeerFileResumeResult(std::shared_ptr<PeerConnection> peerVal, QUuid connectionIdVal,
                         quint64 requestIdVal, QByteArray fileIdVal,
                         FileResumeStatus statusVal, quint64 nextOffsetVal,
                         FileCancelReason errorVal)
        : PeerReq{peerVal, std::move(connectionIdVal), requestIdVal}
        , fileId{std::move(fileIdVal)}, status{statusVal}
        , nextOffset{nextOffsetVal}, error{errorVal} {}

    QByteArray fileId;
    FileResumeStatus status = FileResumeStatus::Rejected;
    quint64 nextOffset = 0;
    FileCancelReason error = FileCancelReason::UserAction;
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
                QString type,
                QByteArray sha512)
    : PeerReq{peer, std::move(connectionId), requestId}
    {
        this->conversation = std::move(conversation);
        this->fileId = std::move(fileId);
        this->name = std::move(name);
        this->size = size;
        this->type = type;
        this->sha512 = std::move(sha512);
    }

    QByteArray conversation;
    QByteArray fileId;
    QString name;
    qlonglong size;
    QString type;
    QByteArray sha512;
};

struct PeerUserInfo : public PeerReq
{
    PeerUserInfo(const PeerUserInfo&) = default;
    PeerUserInfo(std::shared_ptr<PeerConnection> peer, QUuid connectionId, quint64 requestId,
                 UserInfo userInfoVal)
    : PeerReq{peer, std::move(connectionId), requestId}
    {
        userInfo = std::move(userInfoVal);
    }

    UserInfo userInfo;
};

struct PeerBio : public PeerReq
{
    PeerBio(std::shared_ptr<PeerConnection> peerVal, QUuid connectionIdVal,
            quint64 requestIdVal, QString bioVal)
        : PeerReq{std::move(peerVal), std::move(connectionIdVal), requestIdVal}
        , bio{std::move(bioVal)} {}
    QString bio;
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
    virtual uint64_t sendConnectionResponse(quint64 requestId,
                                            ConnectionResponseStatus status,
                                            const QString& message = {}) = 0;
    virtual uint64_t sendMessageReceipt(const QByteArray& messageId) = 0;
    virtual uint64_t sendFileOfferResponse(const QByteArray& fileId,
                                           FileOfferResponseStatus status) = 0;
    virtual uint64_t sendFileAck(const QByteArray& fileId, quint64 nextSequence,
                                 quint64 nextOffset, bool final) = 0;
    virtual uint64_t sendFileCancel(const QByteArray& fileId,
                                    FileCancelReason reason) = 0;
    virtual uint64_t sendFileResume(const QByteArray& fileId, quint64 nextOffset,
                                    const QByteArray& sha256, quint64 fileSize) = 0;
    virtual uint64_t sendFileResumeResult(quint64 responseToRequestId,
                                          const QByteArray& fileId,
                                          FileResumeStatus status,
                                          quint64 nextOffset,
                                          FileCancelReason error) = 0;
    virtual bool isConnected() const noexcept = 0;
    virtual uint64_t sendUserInfo(const core::UserInfo &userInfo) = 0;
    virtual uint64_t requestBio() = 0;
    virtual uint64_t sendBioResponse(quint64 responseToRequestId,
                                     const QString& bio) = 0;
    virtual uint64_t sendMessage(const Message& message) = 0;
    virtual uint64_t sendAvatar(const QImage& avatar) = 0;
    virtual uint64_t requestAvatar() = 0;
    virtual uint64_t sendAvatarResponse(quint64 responseToRequestId,
                                        const QImage& avatar,
                                        bool authorized) = 0;
    virtual uint64_t offerFile(const File& file) = 0;
    virtual uint64_t startTransfer(File& file) = 0;
    virtual uint64_t sendSome(File& file) = 0;
    virtual void disableNotifications() = 0;

signals:
    void connectedToPeer(const std::shared_ptr<PeerConnection>& peer);
    void disconnectedFromPeer(const std::shared_ptr<PeerConnection>& peer);
    void addmeRequest(const PeerAddmeReq& req);
    void receivedConnectionResponse(const PeerConnectionResponse& response);
    void receivedMessageReceipt(const PeerMessageReceipt& receipt);
    void receivedFileOfferResponse(const PeerFileOfferResponse& response);
    void receivedFileAck(const PeerFileAck& ack);
    void receivedFileCancel(const PeerFileCancel& cancel);
    void receivedFileResume(const PeerFileResume& resume);
    void receivedFileResumeResult(const PeerFileResumeResult& result);
    void receivedMessage(const PeerMessage& msg);
    void receivedFileOffer(const PeerFileOffer& msg);
    void receivedAvatar(const PeerSetAvatarReq& avatar);
    void receivedAvatarRequest(const PeerReq& request);
    void receivedAvatarResponse(const PeerSetAvatarReq& avatar);
    void receivedUserInfo(const PeerUserInfo& uinfo);
    void receivedBioRequest(const PeerReq& request);
    void receivedBio(const PeerBio& bio);
    void outputBufferEmptied();
};

}}

Q_DECLARE_METATYPE(ds::core::PeerConnection *)
//Q_DECLARE_SMART_POINTER_METATYPE(std::shared_ptr)
//Q_DECLARE_METATYPE(ds::core::PeerConnection::ptr_t)
Q_DECLARE_METATYPE(ds::core::PeerReq)
Q_DECLARE_METATYPE(ds::core::PeerBio)
Q_DECLARE_METATYPE(ds::core::PeerAddmeReq)
Q_DECLARE_METATYPE(ds::core::PeerConnectionResponse)
Q_DECLARE_METATYPE(ds::core::PeerMessageReceipt)
Q_DECLARE_METATYPE(ds::core::PeerFileOfferResponse)
Q_DECLARE_METATYPE(ds::core::PeerFileAck)
Q_DECLARE_METATYPE(ds::core::PeerFileCancel)
Q_DECLARE_METATYPE(ds::core::PeerFileResume)
Q_DECLARE_METATYPE(ds::core::PeerFileResumeResult)

#endif // PEERCONNECTION_H
