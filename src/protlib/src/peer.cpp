#include <array>
#include <algorithm>
#include <vector>
#include <cassert>
#include <sodium.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include "ds/peer.h"
#include "ds/message.h"
#include "ds/errors.h"
#include "ds/file.h"
#include "ds/dsengine.h"
#include "ds/imageutil.h"
#include "ds/bytes.h"

#include "logfault/logfault.h"

namespace ds {
namespace prot {

using namespace std;
using namespace core;

namespace  {
const std::vector<QString> encoding_names = {"us-ascii", "utf-8"};
const std::map<QString, Message::Encoding>  encoding_lookup = {
    {"us-ascii", Message::US_ACSII},
    {"utf-8", Message::UTF8}        
};

class IncomingFileChannel : public Peer::Channel {
public:
    IncomingFileChannel(const core::File::ptr_t& file)
        : io_{file->getDownloadPath()}
        , file_{file}
    {
        assert(file->getDirection() == File::INCOMING);
        if (!io_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            LFLOG_ERROR << "Failed to open \"" << file->getDownloadPath()
                        << "\" for write: " << io_.errorString();
            throw Error("Failed to open file");
        }

        file->setBytesTransferred(0);

        LFLOG_DEBUG << "Opened file #" << file->getId()
                    << " with path \"" << file->getDownloadPath()
                    << " for WRITE for incoming transfer";
    }

    // Channel interface
public:
    void onIncoming(Peer &peer, const quint64 id,
                    const Peer::mview_t& data,
                    const bool final) override {
        Q_UNUSED(peer);
        if (io_.write(reinterpret_cast<const char *>(data.cdata()),
                      static_cast<qint64>(data.size())) != static_cast<qint64>(data.size())) {
            LFLOG_ERROR << "Failed to write chunk "
                        << id << " to \"" << file_->getDownloadPath()
                        << "\" for write: " << io_.errorString();
            throw Error("Failed to write to file");
        }

        file_->addBytesTransferred(data.size());

        if (final) {
            io_.flush();
            io_.close();
            file_->validateHash();
        }
    }

    uint64_t onOutgoing(Peer &peer) override {
        Q_UNUSED(peer)
        assert(false);
        return {};
    }

private:
    QFile io_;
    File::ptr_t file_;
};

class OutgoingFileChannel : public Peer::Channel {
public:
    OutgoingFileChannel(const core::File::ptr_t& file)
        : io_{file->getPath()}
        , file_{file}
    {
        assert(file->getDirection() == File::OUTGOING);
        if (!io_.open(QIODevice::ReadOnly | QIODevice::ExistingOnly)) {
            LFLOG_ERROR << "Failed to open \"" << file->getPath()
                        << "\" for read: " << io_.errorString();
            throw Error("Failed to open file");
        }

        file->setBytesTransferred(0);

        LFLOG_DEBUG << "Opened file #" << file->getId()
                    << " with path \"" << file->getPath()
                    << " for READ for outgoing transfer";
    }

    // Channel interface
public:
    void onIncoming(Peer &peer, const quint64 id, const Peer::mview_t &data,
                    const bool final) override {
        Q_UNUSED(peer);
        Q_UNUSED(id)
        Q_UNUSED(data)
        Q_UNUSED(final)
        assert(false);
    }

    uint64_t onOutgoing(Peer &peer) override {

        auto bytesRead = io_.read(buffer_.data(), static_cast<int>(buffer_.size()));
        if (bytesRead < 0) {
            LFLOG_ERROR << "Failed to read chunk from file \"" << file_->getPath()
                        << "\": " << io_.errorString();
            file_->transferFailed("Disk Read Error");
            return {};
        }

        const bool finished = io_.atEnd();

        auto rval = peer.send(buffer_.data(),
                  static_cast<size_t>(bytesRead),
                  file_->getChannel(), finished);

        file_->addBytesTransferred(static_cast<size_t>(bytesRead));

        if (finished) {
            file_->transferComplete();
        }

        return rval;
    }

private:
    QFile io_;
    File::ptr_t file_;
    std::array<char, 1024 * 8> buffer_ = {};
};


Message::Encoding toEncoding(const QString& name) {
    const auto it = encoding_lookup.find(name);
    if (it == encoding_lookup.end()) {
        throw NotFoundError(name);
    }
    return it->second;
}

} // anonymous namespace

Peer::Peer(ConnectionSocket::ptr_t connection,
           core::ConnectData connectionData)
    : connection_{move(connection)}, connectionData_{move(connectionData)}
    , uuid_{connection_->getUuid()}
{
    useConnection(connection.get());
    connect(this, &Peer::closeLater,
            this, &Peer::onCloseLater,
            Qt::QueuedConnection);

    connect(this, &Peer::removeTransfer,
            this, [this](core::File::Direction direction, const quint32 id){

        LFLOG_TRACE << "Removing channel #" << id
                    << " from connection " << getConnectionId().toString();

        if (direction == File::INCOMING) {
            inChannels_.erase(id);
        } else {
            outChannels_.erase(id);
        }
    }, Qt::QueuedConnection);
}

uint64_t Peer::send(const QJsonDocument &json)
{
    if (!connection_->isOpen()) {
        throw runtime_error("Connection is closed");
    }

    auto jsonData = json.toJson(QJsonDocument::Compact);

    LFLOG_TRACE << "Sending json to "
                << connection_->getUuid().toString()
                << ": "
                << jsonData;

    return send(jsonData.constData(),
                static_cast<size_t>(jsonData.size()),
                /* channel */ 0);
}

uint64_t Peer::send(const void *data, const size_t bytes,
                    const quint32 ch, const bool eof )
{
    const unsigned char tag = eof
            ? crypto_secretstream_xchacha20poly1305_TAG_PUSH
            : crypto_secretstream_xchacha20poly1305_TAG_MESSAGE;

    if (!connection_->isOpen()) {
        throw runtime_error("Connection is closed");
    }

    // Data format:
    // Two bytes length | one byte version | four bytes channel | 8 bytes id | data

    // The length is encrypted individually to allow the peer to read it before
    // fetching the payload.

    const size_t len = 1 + 4 + 8 + static_cast<size_t>(bytes);
    vector<uint8_t> payload_len(2),
            buffer(len),
            cipherlen(2 + crypt_bytes),
            ciphertext(len + crypt_bytes);
    mview_t version{buffer.data(), 1};
    mview_t channel{version.end(), 4};
    mview_t id{channel.end(), 8};
    mview_t payload{id.end(), bytes};

    assert(buffer.size() == (+ version.size()
                             + channel.size()
                             + id.size()
                             + payload.size()));

    static_assert(sizeof(decltype(qToBigEndian(static_cast<quint16>(len)))) == sizeof(quint16),
                  "qToBigEndian() must return the correct type");

    valueToBytes(qToBigEndian(static_cast<quint16>(len)), payload_len);

    version.at(0) = '\1';

    valueToBytes(qToBigEndian(static_cast<quint32>(ch)), channel);
    valueToBytes(qToBigEndian(static_cast<quint64>(++request_id_)), id);

    assert(bytes == payload.size());
    memcpy(payload.data(), data, payload.size());

    // encrypt length
    if (crypto_secretstream_xchacha20poly1305_push(&stateOut,
                                               cipherlen.data(),
                                               nullptr,
                                               payload_len.data(),
                                               payload_len.size(),
                                               nullptr, 0, 0) != 0) {
        throw runtime_error("Stream encryption failed");
    }

    connection_->write(cipherlen);

    LFLOG_TRACE << "Sending chunk #"
                << request_id_
                << " with payload of "
                << payload.size() << " bytes on channel #" << ch
                << " to connection "<< connection_->getUuid().toString();

    // Encrypt the payload
    if (crypto_secretstream_xchacha20poly1305_push(&stateOut,
                                               ciphertext.data(),
                                               nullptr,
                                               buffer.data(),
                                               buffer.size(),
                                               nullptr, 0, tag) != 0) {
        throw runtime_error("Stream encryption failed");
    }

    connection_->write(ciphertext);
    return request_id_;
}

void Peer::onReceivedData(const quint32 channel, const quint64 id,
                          const Peer::mview_t& data, const bool final)
{
    if (channel == 0) {
        onReceivedJson(channel, data);
    } else {
        auto it = inChannels_.find(channel);
        if (it == inChannels_.end()) {
            LFLOG_WARN << "Data to unknown channel #" << channel
                       << " on connection " << getConnectionId().toString();

            // TODO: What do we do about it?
            return;
        }

        Channel::ptr_t channelInstance;
        if (final) {
            // The state change in inIncoming with final will erase the channel, so we need
            // an extra instance to stay safe until onIncoming() returns;
            channelInstance = it->second;
        }

        it->second->onIncoming(*this, id, data, final);

        if (final) {
            LFLOG_TRACE << "Transfer om channel #" << channel
                        << " on connection " << getConnectionId().toString()
                        << " is completed.";
        }
    }
}

void Peer::onReceivedJson(const quint64 id, const Peer::mview_t& data)
{
    if (notificationsDisabled_) {
        return;
    }

    // Control channel. Data is supposed to be Json.
    QJsonDocument json = QJsonDocument::fromJson(data.toByteArray());
    if (json.isNull()) {
        LFLOG_ERROR << "Incoming data on " << getConnectionId().toString()
                    << " with id=" << id
                    << " is supposed to be in Json format, but it is not.";
        throw Error("Not Json");
    }

    const auto type = json.object().value("type");

    if (type == "AddMe") {
        PeerAddmeReq req{shared_from_this(), getConnectionId(), id,
                    json.object().value("nick").toString(),
                    json.object().value("message").toString(),
                    json.object().value("address").toString().toUtf8(),
                    getPeerCert()->getB58PubKey()};

        LFLOG_TRACE << "Emitting addmeRequest";
        emit addmeRequest(req);
    } else if (type == "Ack") {

        QVariantMap params;
        for(const auto& key : json.object().keys()) {
            static const QRegExp irrelevant{"what|status|type"};
            if (key.count(irrelevant)) {
                continue;
            }
            params.insert(key, json.object().value(key).toString());
        }

        PeerAck ack{shared_from_this(), getConnectionId(), id,
                    json.object().value("what").toString().toUtf8(),
                    json.object().value("status").toString().toUtf8(),
                    params};

        LFLOG_TRACE << "Emitting Ack";
        emit receivedAck(ack);
    } else if (type == "Message") {
        PeerMessage msg{shared_from_this(), getConnectionId(), id,
                    QByteArray::fromBase64(json.object().value("conversation").toString().toUtf8()),
                    QByteArray::fromBase64(json.object().value("message-id").toString().toUtf8()),
                    QDateTime::fromString(json.object().value("date").toString(), Qt::ISODate),
                    json.object().value("content").toString(),
                    QByteArray::fromBase64(json.object().value("from").toString().toUtf8()),
                    toEncoding(json.object().value("encoding").toString()),
                    QByteArray::fromBase64(json.object().value("signature").toString().toUtf8())};

        LFLOG_TRACE << "Emitting PeerMessage";
        emit receivedMessage(msg);
    } else if (type == "IncomingFile") {
        PeerFileOffer msg{shared_from_this(), getConnectionId(), id,
                    QByteArray::fromBase64(json.object().value("conversation").toString().toUtf8()),
                    QByteArray::fromBase64(json.object().value("file-id").toString().toUtf8()),
                    json.object().value("name").toString(),
                    json.object().value("size").toString().toLongLong(),
                    json.object().value("rest").toString().toLongLong(),
                    json.object().value("file-type").toString(),
                    QByteArray::fromBase64(json.object().value("sha256").toString().toUtf8())};

        LFLOG_TRACE << "Emitting PeerFileOffer";
        emit receivedFileOffer(msg);
    } else if (type == "SetAvatar") {
        PeerSetAvatarReq avatar{shared_from_this(), getConnectionId(), id,
                    toQimage(json.object())};

        LFLOG_TRACE << "Emitting PeerSetAvatarReq";
        emit receivedAvatar(avatar);
    } else {
        LFLOG_WARN << "Unrecognized request from peer at connection "
                   << getConnectionId().toString();
    }
}

void Peer::onCloseLater()
{
    if (connection_->isOpen()) {
        connection_->close();
    }

    if (!notificationsDisabled_) {
        emit disconnectedFromPeer(shared_from_this());
    }
}

void Peer::enableEncryptedStream()
{
    if (inState_ == InState::CLOSING) {
        return;
    }

    assert(inState_ == InState::DISABLED);
    wantChunkSize();
}

void Peer::wantChunkSize()
{
    if (inState_ == InState::CLOSING) {
        return;
    }

    LFLOG_TRACE << "Want chunk-len bytes (2) on " << connection_->getUuid().toString();
    inState_ = InState::CHUNK_SIZE;
    connection_->wantBytes(2 + crypt_bytes);
}

void Peer::wantChunkData(const size_t bytes)
{
    if (inState_ == InState::CLOSING) {
        return;
    }

    LFLOG_TRACE << "Want " << bytes << " data-bytes on " << connection_->getUuid().toString();
    inState_ = InState::CHUNK_DATA;
    connection_->wantBytes(bytes + crypt_bytes);
}

// Incoming, stream-encrypted data
void Peer::processStream(const Peer::data_t &ciphertext)
{
    if (inState_ == InState::CLOSING) {
        return;
    }

    bool final = {};
    if (inState_ == InState::CHUNK_SIZE) {
        array<uint8_t, 2> bytes = {};
        mview_t data{bytes};
        decrypt(data, ciphertext, final);
        wantChunkData(qFromBigEndian(bytesToValue<quint16>(bytes)));
    } else if (inState_ == InState::CHUNK_DATA){

        static const QByteArray binary = {"[binary]"};
            assert(ciphertext.size() >= crypt_bytes + 5);
        std::vector<uint8_t> buffer(ciphertext.size() - crypt_bytes);
        mview_t buffer_view{buffer};
        mview_t version{buffer.data(), 1};
        mview_t channel{version.end(), 4};
        mview_t id{channel.end(), 8};

        const int payload_size = static_cast<int>(buffer.size())
                - static_cast<int>(version.size())
                - static_cast<int>(channel.size())
                - static_cast<int>(id.size());

        if (payload_size < 0) {
            throw runtime_error("Payload size underflow");
        }

        mview_t payload{id.end(), static_cast<size_t>(payload_size)};

        assert(buffer.size() == (+ version.size()
                                 + channel.size()
                                 + id.size()
                                 + payload.size()));

        decrypt(buffer_view, ciphertext, final);

        if (version.at(0) != '\1') {
            LFLOG_WARN << "Unknown chunk version" << static_cast<unsigned int>(version.at(0));
            throw runtime_error("Unknown chunk version");
        }

        const auto channel_id = qFromBigEndian(bytesToValue<quint32>(channel));
        const auto chunk_id = qFromBigEndian(bytesToValue<quint64>(id));

        LFLOG_TRACE << "Received chunk on "
                    << connection_->getUuid().toString()
                    << ", size=" << payload.size()
                    << ", channel=" << channel_id
                    << ", id=" << chunk_id
                    << ", payload=" << (channel_id ? binary : safePayload(payload));

        try {
            onReceivedData(channel_id, chunk_id, payload, final);
        } catch (const std::exception& ex) {
            LFLOG_ERROR << "Caught exception while processing incoming message on connection "
                        << getConnectionId().toString()
                        << " :"
                        << ex.what();
            close();
            return;
        }

        wantChunkSize();
    } else {
        throw runtime_error("Unexpected InState");
    }
}

void Peer::prepareEncryption(Peer::stream_state_t &state,
                             mview_t& header,
                             Peer::mview_t &key)
{
    crypto_secretstream_xchacha20poly1305_keygen(key.data());

    if (crypto_secretstream_xchacha20poly1305_init_push(&state, header.data(), key.cdata()) != 0) {
        throw runtime_error("Failed to initialize encryption");
    }

}

void Peer::prepareDecryption(Peer::stream_state_t &state,
                             const mview_t &header,
                             const Peer::mview_t &key)
{
    if (crypto_secretstream_xchacha20poly1305_init_pull(&state, header.cdata(), key.cdata()) != 0) {
        LFLOG_WARN << "Invalid decryption key / header from: " << connection_->getUuid().toString();
        throw runtime_error("Failed to initialize decryption");
    }
}

void Peer::decrypt(Peer::mview_t &data, const Peer::mview_t &ciphertext,  bool& final)
{
    assert((data.size() + crypt_bytes) == ciphertext.size());
    unsigned char tag = {};
    if (crypto_secretstream_xchacha20poly1305_pull(&stateIn,
                                                   data.data(),
                                                   nullptr,
                                                   &tag,
                                                   ciphertext.cdata(),
                                                   ciphertext.size(),
                                                   nullptr, 0) != 0) {
        throw runtime_error("Decryption of stream failed");
    }

    final = (tag == crypto_secretstream_xchacha20poly1305_TAG_PUSH);

    if (!final && (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL)) {

        // NOTE: Currently we dont use this feature, so this is not supposed to happen.

        LFLOG_TRACE << "Received tag 'FINAL' on " << connection_->getUuid().toString()
                    << ". Closing connection";

        connection_->close();
    }
}

QByteArray Peer::safePayload(const Peer::mview_t &data)
{
    const auto json_text = data.toByteArray();
    QJsonDocument json = QJsonDocument::fromJson(json_text);
    if (!json.isNull()) {
        return json_text;
    }

    return "*** NOT Json ***";
}

quint32 Peer::createChannel(const File &file)
{
    quint32 channelId = 0;
    auto filePtr = core::DsEngine::instance().getFileManager()->getFile(file.getId());
    Channel::ptr_t ch;
    if (file.getDirection() == File::INCOMING) {
        ch = make_shared<IncomingFileChannel>(filePtr);
        assert(inChannels_.find(nextInchannel_) == inChannels_.end());
        channelId = nextInchannel_;
        inChannels_[channelId] = ch;
        ++nextInchannel_;
    } else {
        channelId = file.getChannel();
        assert(channelId > 0);
        assert(outChannels_.find(channelId) == outChannels_.end());
        ch = make_shared<OutgoingFileChannel>(filePtr);
        outChannels_[channelId] = ch;
    }

    const auto direction = file.getDirection();
    auto fileCPtr = filePtr.get();
    connect(fileCPtr, &File::stateChanged,
            this, [this, channelId, direction, fileCPtr]() {

        if (fileCPtr->getState() != File::FS_TRANSFERRING) {
            emit removeTransfer(direction, channelId);
        }
    });

    return channelId;
}

uint64_t Peer::startReceive(File &file)
{
    auto channelId = createChannel(file);

    LFLOG_DEBUG << "Preparing to start receiving file #" << file.getId()
                << " \"" << file.getName()
                << "\" of size "
                << file.getSize()
                << " from " << file.getContact()->getName()
                << " on identiy "
                << file.getConversation()->getIdentity()->getName()
                << " with channel #"
                << channelId;

    const auto params = QVariantMap {
            {"rest", QString::number(0)},
            {"data", QString{file.getFileId().toBase64()}},
            {"channel", channelId}
    };

    LFLOG_DEBUG << "Requesting File : " << file.getId()
                << " with channel #" << channelId
                << " over connection " << getConnectionId().toString();

    const auto rval = sendAck("IncomingFile", "Proceed", params);
    file.clearBytesTransferred();
    file.setState(File::FS_TRANSFERRING);
    file.setChannel(channelId);
    return rval;
}

uint64_t Peer::startSend(File &file)
{
    auto channelId = createChannel(file);
    file.clearBytesTransferred();
    file.setState(File::FS_TRANSFERRING);
    return outChannels_.at(channelId)->onOutgoing(*this);
}

void Peer::useConnection(ConnectionSocket *cc)
{
    Q_UNUSED(cc);
    connect(connection_.get(), &ConnectionSocket::connected,
            this, [this]() {

        LFLOG_DEBUG << "Peer " << getConnectionId().toString()
                    << " is connected";
    });

    connect(connection_.get(), &ConnectionSocket::disconnected,
            this, [this]() {

        LFLOG_DEBUG << "Peer " << getConnectionId().toString()
                    << " is disconnected";

        if (!notificationsDisabled_) {
            emit disconnectedFromPeer(shared_from_this());
        }
    });

    connect(connection_.get(), &ConnectionSocket::outputBufferEmptied,
            this, [this]() {

        if (!notificationsDisabled_) {
            emit outputBufferEmptied();
        }
    }, Qt::QueuedConnection);
}

QUuid Peer::getConnectionId() const
{
    return uuid_;
}

crypto::DsCert::ptr_t Peer::getPeerCert() const noexcept
{
    return connectionData_.contactsCert;
}

void Peer::close()
{
   inState_ = InState::CLOSING;
   emit closeLater();
}

QUuid Peer::getIdentityId() const noexcept
{
    return connectionData_.service;
}

uint64_t Peer::sendAck(const QString &what, const QString &status, const QString& data)
{
    const QVariantMap param = {
        {"data", data}
    };

    return sendAck(what, status, param);
}

uint64_t Peer::sendAck(const QString &what, const QString &status, const QVariantMap &params)
{
    auto object = QJsonObject{
        {"type", "Ack"},
        {"what", what},
        {"status", status},
    };

    for(auto it = params.constBegin(); it != params.constEnd(); ++it) {
        object.insert(it.key(), it.value().toString());
    }

    LFLOG_DEBUG << "Sending Ack: " << what
                << " with status: " << status
                << " over connection " << getConnectionId().toString();

    return send(QJsonDocument{object});
}

bool Peer::isConnected() const noexcept
{
    return connection_ && connection_->isOpen();
};

uint64_t Peer::sendMessage(const core::Message &message)
{
    auto json = QJsonDocument{
        QJsonObject{
            {"type", "Message"},
            {"message-id", QString{message.getData().messageId.toBase64()}},
            {"date", message.getData().composedTime.toString(Qt::ISODate)},
            {"content", message.getData().content},
            {"encoding", encoding_names.at(static_cast<size_t>(message.getData().encoding))},
            {"conversation", QString{message.getData().conversation.toBase64()}},
            {"from", QString{message.getData().sender.toBase64()}},
            {"signature", QString{message.getData().signature.toBase64()}}
        }
    };

    LFLOG_DEBUG << "Sending Message: " << message.getId()
                << " over connection " << getConnectionId().toString();

    return send(json);
}

uint64_t Peer::sendAvatar(const QImage &avatar)
{
    auto obj = toJson(avatar);
    obj.insert("type", "SetAvatar");
    auto json = QJsonDocument{
        QJsonObject{ move(obj) }
    };

    LFLOG_DEBUG << "Sending Avatar over connection " << getConnectionId().toString();

    return send(json);
}

uint64_t Peer::offerFile(const File &file)
{
    auto json = QJsonDocument{
        QJsonObject{
            {"type", "IncomingFile"},
            {"sha256", QString{file.getHash().toBase64()}},
            {"name", file.getName()},
            {"size", QString::number(file.getSize())},
            {"file-type", "binary"},
            {"rest", QString::number(0)},
            {"file-id", QString{file.getFileId().toBase64()}},
            {"conversation", QString{file.getConversation()->getHash().toBase64()}},
        }
    };

    LFLOG_DEBUG << "Sending File Offer for file: " << file.getId()
                << " over connection " << getConnectionId().toString();

    return send(json);
}

uint64_t Peer::startTransfer(File &file)
{
    if (file.getDirection() == File::INCOMING) {
        return startReceive(file);
    }

    return startSend(file);

}

uint64_t Peer::sendSome(File &file)
{
    assert(file.getState() == File::FS_TRANSFERRING);

    auto it = outChannels_.find(file.getChannel());
    if (it == outChannels_.end()) {
        LFLOG_ERROR << "Asked to send data for file #"
                    << file.getId()
                    << " on channel "
                    << file.getChannel()
                    << " but I don't have that channel!";

        file.transferFailed("No active channel to send to");
        return {};
    }

    auto instance = it->second;
    return instance->onOutgoing(*this);
}

void Peer::disableNotifications()
{
    notificationsDisabled_ = true;
}


}} // namespace

