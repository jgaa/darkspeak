#include <array>
#include <algorithm>
#include <vector>
#include <map>
#include <chrono>
#include <cassert>
#include <regex>

#include <sodium.h>

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QRandomGenerator>
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
        if (file->getDirection() != File::INCOMING) {
            throw Error("Invalid direction for incoming file channel");
        }
        const auto partialPath = file->getDownloadPath();
        const auto metadataPath = partialPath + QStringLiteral(".meta");
        QFileInfo partialInfo{partialPath};
        const auto metadata = readMetadata(metadataPath);
        bool resume = false;
        quint64 savedOffset = {};
        if (!metadata.isEmpty()
                && metadata.value("fileId").toString()
                    == QString::fromLatin1(file->getFileId().toBase64())
                && metadata.value("size").toString() == QString::number(file->getSize())
                && metadata.value("hash").toString()
                    == QString::fromLatin1(file->getHash().toBase64())) {
            bool ok = false;
            savedOffset = metadata.value("offset").toString().toULongLong(&ok);
            const auto prefixHash = metadata.value("prefixHash").toString().toLatin1();
            resume = ok && savedOffset > 0 && savedOffset <= static_cast<quint64>(file->getSize())
                    && savedOffset % buffer_bytes == 0
                    && partialInfo.exists() && partialInfo.size() == static_cast<qint64>(savedOffset)
                    && prefixHash == hashPrefix(partialPath).toBase64();
        }

        if (resume) {
            if (!io_.open(QIODevice::WriteOnly | QIODevice::Append)) {
                throw Error("Failed to resume file");
            }
            nextOffset_ = savedOffset;
            nextSequence_ = savedOffset / buffer_bytes;
        } else {
            QFile::remove(partialPath);
            QFile::remove(metadataPath);
            if (!io_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                throw Error("Failed to open file");
            }
        }

        initializePrefixHash();
        file->setBytesTransferred(static_cast<qlonglong>(nextOffset_));
        saveMetadata();

        LFLOG_DEBUG << "Opened file #" << file->getId()
                    << " with path \"" << file->getDownloadPath()
                    << " for WRITE for incoming transfer";
    }

    // Channel interface
public:
    void onIncoming(Peer &peer, const quint64 id,
                    const Peer::mview_t& data,
                    const bool final) override {
        if (data.size() < Peer::file_chunk_header_bytes) {
            throw Error("File chunk is smaller than its header");
        }

        quint64 sequence = {};
        quint64 offset = {};
        memcpy(&sequence, data.cdata(), sizeof(sequence));
        memcpy(&offset, data.cdata() + sizeof(sequence), sizeof(offset));
        sequence = qFromBigEndian(sequence);
        offset = qFromBigEndian(offset);

        if (sequence < nextSequence_) {
            if (finalReceived_ && file_->getState() == File::FS_HASHING) {
                return;
            }
            sendAck(peer, nextSequence_ - 1, nextOffset_, finalReceived_);
            return;
        }
        if (sequence != nextSequence_ || offset != nextOffset_) {
            throw Error("File chunk is outside the receive window");
        }

        const auto payloadSize = data.size() - Peer::file_chunk_header_bytes;
        if (payloadSize > Peer::file_chunk_bytes) {
            throw Error("File chunk exceeds the negotiated chunk size");
        }
        const auto fileSize = static_cast<quint64>(file_->getSize());
        if (nextOffset_ > fileSize
                || payloadSize > fileSize - nextOffset_) {
            throw Error("File chunk exceeds the announced file size");
        }
        if (payloadSize == 0 && (!final || nextOffset_ != fileSize)) {
            throw Error("Empty non-final file chunk");
        }
        if (final && nextOffset_ + payloadSize != fileSize) {
            throw Error("Final file chunk has an invalid size");
        }
        const auto payload = reinterpret_cast<const char *>(
            data.cdata() + Peer::file_chunk_header_bytes);
        if (io_.write(payload, static_cast<qint64>(payloadSize))
                != static_cast<qint64>(payloadSize)) {
            LFLOG_ERROR << "Failed to write chunk "
                        << id << " to \"" << file_->getDownloadPath()
                        << "\" for write: " << io_.errorString();
            file_->transferFailed(QStringLiteral("Failed to write received file: ")
                                  + io_.errorString(), File::FS_FAILED);
            return;
        }

        ++nextSequence_;
        nextOffset_ += static_cast<quint64>(payloadSize);
        crypto_hash_sha256_update(&prefixState_,
                                  reinterpret_cast<const unsigned char *>(payload),
                                  static_cast<size_t>(payloadSize));
        file_->setBytesTransferred(static_cast<qlonglong>(nextOffset_));
        try {
            saveMetadata();
        } catch (const std::exception& ex) {
            file_->transferFailed(QStringLiteral("Failed to save received file state: ")
                                  + ex.what(), File::FS_FAILED);
            return;
        }
        finalReceived_ = final;

        if (final) {
            if (!io_.flush()) {
                file_->transferFailed(QStringLiteral("Failed to flush received file: ")
                                      + io_.errorString(), File::FS_FAILED);
                return;
            }
            io_.close();
            pendingFinal_ = true;
            finalSequence_ = sequence;
            finalOffset_ = nextOffset_;
            file_->validateHash();
        } else {
            sendAck(peer, sequence, nextOffset_, false);
        }
    }

    uint64_t onOutgoing(Peer &peer) override {
        Q_UNUSED(peer)
        throw Error("Incoming file channel cannot send data");
        return {};
    }

    qlonglong resumeOffset() const noexcept override
    {
        return static_cast<qlonglong>(nextOffset_);
    }

    void onTransferDone(Peer& peer, bool success) override
    {
        if (!success || !pendingFinal_) {
            return;
        }
        try {
            sendAck(peer, finalSequence_, finalOffset_, true);
        } catch (const std::exception& ex) {
            LFLOG_WARN << "Failed to send final file acknowledgement: " << ex.what();
        }
        pendingFinal_ = false;
    }

private:
    static constexpr quint64 buffer_bytes = Peer::file_chunk_bytes;

    static QJsonObject readMetadata(const QString& path)
    {
        QFile metadataFile{path};
        if (!metadataFile.open(QIODevice::ReadOnly)) {
            return {};
        }
        const auto document = QJsonDocument::fromJson(metadataFile.readAll());
        return document.isObject() ? document.object() : QJsonObject{};
    }

    static QByteArray hashPrefix(const QString& path)
    {
        QFile input{path};
        if (!input.open(QIODevice::ReadOnly)) {
            return {};
        }
        crypto_hash_sha256_state state{};
        crypto_hash_sha256_init(&state);
        std::array<unsigned char, buffer_bytes> buffer{};
        while (!input.atEnd()) {
            const auto bytes = input.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
            if (bytes < 0) {
                return {};
            }
            crypto_hash_sha256_update(&state, buffer.data(), static_cast<size_t>(bytes));
        }
        QByteArray hash(crypto_hash_sha256_BYTES, Qt::Uninitialized);
        crypto_hash_sha256_final(&state, reinterpret_cast<unsigned char *>(hash.data()));
        return hash;
    }

    void initializePrefixHash()
    {
        crypto_hash_sha256_init(&prefixState_);
        QFile input{file_->getDownloadPath()};
        if (!input.open(QIODevice::ReadOnly)) {
            throw Error("Failed to open partial file for hashing");
        }
        std::array<unsigned char, buffer_bytes> buffer{};
        while (!input.atEnd()) {
            const auto bytes = input.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
            if (bytes < 0) {
                throw Error("Failed to read partial file for hashing");
            }
            crypto_hash_sha256_update(&prefixState_, buffer.data(), static_cast<size_t>(bytes));
        }
    }

    QByteArray currentPrefixHash() const
    {
        auto state = prefixState_;
        QByteArray hash(crypto_hash_sha256_BYTES, Qt::Uninitialized);
        crypto_hash_sha256_final(&state, reinterpret_cast<unsigned char *>(hash.data()));
        return hash;
    }

    void saveMetadata()
    {
        QSaveFile metadataFile{file_->getDownloadPath() + QStringLiteral(".meta")};
        if (!metadataFile.open(QIODevice::WriteOnly)) {
            throw Error("Failed to save transfer metadata");
        }
        const QJsonObject metadata{
            {"fileId", QString::fromLatin1(file_->getFileId().toBase64())},
            {"size", QString::number(file_->getSize())},
            {"hash", QString::fromLatin1(file_->getHash().toBase64())},
            {"offset", QString::number(nextOffset_)},
            {"prefixHash", QString::fromLatin1(currentPrefixHash().toBase64())}
        };
        metadataFile.write(QJsonDocument{metadata}.toJson(QJsonDocument::Compact));
        if (!metadataFile.commit()) {
            throw Error("Failed to commit transfer metadata");
        }
    }

    void sendAck(Peer& peer, quint64 sequence, quint64 offset, bool final)
    {
        peer.sendAck("IncomingFile", "ChunkAck", QVariantMap{
            {"data", file_->getFileId().toBase64()},
            {"sequence", QString::number(sequence)},
            {"offset", QString::number(offset)},
            {"final", final ? QStringLiteral("1") : QStringLiteral("0")}
        });
    }

    QFile io_;
    File::ptr_t file_;
    crypto_hash_sha256_state prefixState_{};
    quint64 nextSequence_ = {};
    quint64 nextOffset_ = {};
    bool finalReceived_ = false;
    bool pendingFinal_ = false;
    quint64 finalSequence_ = {};
    quint64 finalOffset_ = {};
};

class OutgoingFileChannel : public Peer::Channel {
public:
    OutgoingFileChannel(const core::File::ptr_t& file)
        : io_{file->getPath()}
        , file_{file}
    {
        if (file->getDirection() != File::OUTGOING) {
            throw Error("Invalid direction for outgoing file channel");
        }
        if (!io_.open(QIODevice::ReadOnly)) {
            LFLOG_ERROR << "Failed to open \"" << file->getPath()
                        << "\" for read: " << io_.errorString();
            throw Error("Failed to open file");
        }

        const auto resumeOffset = file->getBytesTransferred();
        if (resumeOffset < 0 || resumeOffset > file->getSize()
                || resumeOffset % static_cast<qlonglong>(buffer_.size()) != 0
                || !io_.seek(resumeOffset)) {
            throw Error("Invalid file resume offset");
        }
        nextOffset_ = static_cast<quint64>(resumeOffset);
        nextSequence_ = nextOffset_ / buffer_.size();

        file->setBytesTransferred(resumeOffset);

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
        throw Error("Outgoing file channel cannot receive data");
    }

    uint64_t onOutgoing(Peer &peer) override {
        uint64_t lastFrame = {};
        while (inFlight_.size() < Peer::file_window_chunks && !endQueued_) {
            const auto bytesRead = io_.read(buffer_.data(), static_cast<int>(buffer_.size()));
            if (bytesRead < 0) {
                file_->transferFailed("Disk Read Error", File::FS_FAILED);
                return {};
            }

            const bool finished = io_.atEnd();
            QByteArray payload;
            payload.resize(static_cast<int>(Peer::file_chunk_header_bytes + bytesRead));
            const auto sequence = nextSequence_++;
            const auto offset = nextOffset_;
            const auto wireSequence = qToBigEndian(sequence);
            const auto wireOffset = qToBigEndian(offset);
            memcpy(payload.data(), &wireSequence, sizeof(wireSequence));
            memcpy(payload.data() + sizeof(wireSequence), &wireOffset, sizeof(wireOffset));
            memcpy(payload.data() + Peer::file_chunk_header_bytes,
                   buffer_.data(), static_cast<size_t>(bytesRead));

            try {
                lastFrame = peer.send(payload.constData(), static_cast<size_t>(payload.size()),
                                      file_->getChannel(), finished);
            } catch (const std::exception& ex) {
                file_->transferFailed(QStringLiteral("Failed to send file chunk: ")
                                      + ex.what(), File::FS_FAILED);
                return {};
            }

            inFlight_.emplace(sequence, PendingChunk{
                payload, finished, 0, std::chrono::steady_clock::now(),
                offset, nextOffset_ + static_cast<quint64>(bytesRead)});
            nextOffset_ += static_cast<quint64>(bytesRead);
            endQueued_ = finished;
        }
        return lastFrame;
    }

    void onAck(Peer& peer, quint64 sequence, qlonglong offset, bool final) override
    {
        Q_UNUSED(peer)
        if (file_->getState() == File::FS_DONE) {
            return;
        }
        if (file_->getState() != File::FS_TRANSFERRING) {
            throw Error("Acknowledgement for inactive file transfer");
        }
        if (sequence >= nextSequence_ || offset < 0
                || static_cast<quint64>(offset) > static_cast<quint64>(file_->getSize())) {
            throw Error("Invalid file acknowledgement");
        }

        // Cumulative ACKs may be repeated. They must describe exactly the
        // same contiguous prefix and may not move the sender's offset.
        if (haveAcknowledgement_ && sequence <= acknowledgedSequence_) {
            if (offset != acknowledgedOffset_ || final) {
                throw Error("Inconsistent duplicate file acknowledgement");
            }
            return;
        }

        const auto pending = inFlight_.find(sequence);
        if (pending == inFlight_.end()
                || offset != static_cast<qlonglong>(pending->second.endOffset)) {
            throw Error("File acknowledgement does not match the chunk");
        }

        auto it = inFlight_.begin();
        while (it != inFlight_.end() && it->first <= sequence) {
            it = inFlight_.erase(it);
        }

        const auto acknowledged = static_cast<qlonglong>(offset);
        acknowledgedSequence_ = sequence;
        acknowledgedOffset_ = acknowledged;
        haveAcknowledgement_ = true;
        if (acknowledged > file_->getBytesTransferred()) {
            file_->setBytesTransferred(acknowledged);
        }

        if (final) {
            if (!endQueued_ || sequence + 1 != nextSequence_ || !inFlight_.empty()
                    || acknowledged != file_->getSize()) {
                throw Error("Invalid final file acknowledgement");
            }
            file_->transferComplete();
        }
    }

    void onTimeout(Peer& peer) override
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto &[sequence, pending] : inFlight_) {
            Q_UNUSED(sequence)
            if (now - pending.sent < std::chrono::seconds(2)) {
                continue;
            }
            if (pending.retries >= Peer::file_retry_limit) {
                file_->transferFailed("File transfer acknowledgement timeout", File::FS_FAILED);
                return;
            }
            try {
                peer.send(pending.payload.constData(), static_cast<size_t>(pending.payload.size()),
                          file_->getChannel(), pending.final);
            } catch (const std::exception& ex) {
                file_->transferFailed(QStringLiteral("Failed to retransmit file chunk: ")
                                      + ex.what(), File::FS_FAILED);
                return;
            }
            pending.sent = now;
            ++pending.retries;
        }
    }

private:
    struct PendingChunk {
        QByteArray payload;
        bool final = false;
        int retries = {};
        std::chrono::steady_clock::time_point sent;
        quint64 offset = {};
        quint64 endOffset = {};
    };

    QFile io_;
    File::ptr_t file_;
    std::array<char, Peer::file_chunk_bytes> buffer_ = {};
    std::map<quint64, PendingChunk> inFlight_;
    quint64 nextSequence_ = {};
    quint64 nextOffset_ = {};
    bool endQueued_ = false;
    bool haveAcknowledgement_ = false;
    quint64 acknowledgedSequence_ = {};
    qlonglong acknowledgedOffset_ = {};
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
    keepAliveTimer_.setSingleShot(true);
    connect(&keepAliveTimer_, &QTimer::timeout,
            this, &Peer::sendKeepAlive);
    auto *transferTimer = new QTimer(this);
    transferTimer->setInterval(1000);
    connect(transferTimer, &QTimer::timeout, this, [this]() {
        for (const auto& [channel, instance] : outChannels_) {
            Q_UNUSED(channel)
            instance->onTimeout(*this);
        }
    });
    transferTimer->start();
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
    resetIdleTimer();

    if (!json.isObject()) {
        throw Error("Control packet must be a JSON object");
    }

    auto object = json.object();
    object.insert("protocol", control_version);
    auto jsonData = QJsonDocument{object}.toJson(QJsonDocument::Compact);

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
    resetIdleTimer();

    // Data format:
    // Two bytes length | one byte version | four bytes channel | 8 bytes id | data

    // The length is encrypted individually to allow the peer to read it before
    // fetching the payload.

    if (static_cast<size_t>(bytes) > max_payload_bytes) {
        throw runtime_error("Payload exceeds encrypted frame limit");
    }

    const size_t len = frame_header_bytes + static_cast<size_t>(bytes);
    vector<uint8_t> payload_len(2),
            buffer(len),
            cipherlen(2 + crypt_bytes),
            ciphertext(len + crypt_bytes);
    const auto wireBytes = cipherlen.size() + ciphertext.size();
    if (connection_->queuedBytes() > ConnectionSocket::maxOutgoingBytes()
            || wireBytes > ConnectionSocket::maxOutgoingBytes()
            - connection_->queuedBytes()) {
        throw runtime_error("Connection output queue limit exceeded");
    }
    mview_t version{buffer.data(), 1};
    mview_t channel{version.end(), 4};
    mview_t id{channel.end(), 8};
    mview_t payload{id.end(), bytes};

    if (buffer.size() != version.size() + channel.size()
            + id.size() + payload.size()) {
        throw runtime_error("Invalid outgoing encrypted frame layout");
    }

    static_assert(sizeof(decltype(qToBigEndian(static_cast<quint16>(len)))) == sizeof(quint16),
                  "qToBigEndian() must return the correct type");

    valueToBytes(qToBigEndian(static_cast<quint16>(len)), payload_len);

    version.at(0) = static_cast<char>(frame_version);

    valueToBytes(qToBigEndian(static_cast<quint32>(ch)), channel);
    valueToBytes(qToBigEndian(static_cast<quint64>(++request_id_)), id);

    if (bytes != payload.size()) {
        throw runtime_error("Invalid outgoing payload size");
    }
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

    // Queue the two wire parts atomically. If the bounded socket queue cannot
    // accept the complete frame, neither its length nor its body is queued.
    QByteArray wireFrame;
    wireFrame.reserve(cipherlen.size() + ciphertext.size());
    wireFrame.append(reinterpret_cast<const char *>(cipherlen.data()),
                     static_cast<qsizetype>(cipherlen.size()));
    wireFrame.append(reinterpret_cast<const char *>(ciphertext.data()),
                     static_cast<qsizetype>(ciphertext.size()));
    connection_->write(wireFrame);
    return request_id_;
}

void Peer::onReceivedData(const quint32 channel, const quint64 id,
                          const Peer::mview_t& data, const bool final)
{
    if (id == 0 || id <= last_received_request_id_) {
        protocolError(QStringLiteral("Non-monotonic frame id"));
    }
    last_received_request_id_ = id;
    resetIdleTimer();

    if (channel == 0) {
        if (final) {
            protocolError(QStringLiteral("Control frame cannot be final"));
        }
        onReceivedJson(id, data);
    } else {
        auto it = inChannels_.find(channel);
        if (it == inChannels_.end()) {
            LFLOG_WARN << "Data to unknown channel #" << channel
                       << " on connection " << getConnectionId().toString();

            protocolError(QStringLiteral("Unknown channel"));
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

    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(data.toByteArray(), &parseError);
    if (json.isNull() || !json.isObject()) {
        LFLOG_ERROR << "Incoming data on " << getConnectionId().toString()
                    << " with id=" << id
                    << " is supposed to be in Json format, but it is not.";
        protocolError(QStringLiteral("Control packet is not a JSON object"));
    }

    const auto object = json.object();
    if (object.value("protocol").toInt(-1) != control_version) {
        protocolError(QStringLiteral("Unsupported control protocol version"));
    }

    const auto type = object.value("type").toString();
    if (type.isEmpty()) {
        protocolError(QStringLiteral("Control packet has no type"));
    }
    if (!capabilitiesReceived_ && type != QStringLiteral("Capabilities")) {
        protocolError(QStringLiteral("Control packet received before capabilities"));
    }

    if (type == QStringLiteral("Capabilities")) {
        if (capabilitiesReceived_) {
            protocolError(QStringLiteral("Duplicate capabilities packet"));
        }
        const auto capabilities = object.value("capabilities");
        if (!capabilities.isArray()
                || !capabilities.toArray().contains(QJsonValue(QStringLiteral("frame-v2")))
                || !capabilities.toArray().contains(QJsonValue(QStringLiteral("file-window-v1")))
                || !capabilities.toArray().contains(QJsonValue(QStringLiteral("keepalive-v1")))
                || object.value("max-frame").toInt(-1) != static_cast<int>(max_frame_bytes)) {
            protocolError(QStringLiteral("Invalid capabilities packet"));
        }
        capabilitiesReceived_ = true;
        return;
    }

    if (type == QStringLiteral("KeepAlive")) {
        const auto nonce = QByteArray::fromBase64(
            object.value("nonce").toString().toUtf8());
        const auto padding = QByteArray::fromBase64(
            object.value("padding").toString().toUtf8());
        if (nonce.size() != 16 || padding.size() > 256) {
            protocolError(QStringLiteral("Invalid keepalive packet"));
        }
        return;
    } else if (type == QStringLiteral("AddMe")) {
        PeerAddmeReq req{shared_from_this(), getConnectionId(), id,
                    object.value("nick").toString(),
                    object.value("message").toString(),
                    object.value("address").toString().toUtf8(),
                    getPeerCert()->getB58PubKey()};

        LFLOG_TRACE << "Emitting addmeRequest";
        emit addmeRequest(req);
    } else if (type == QStringLiteral("Ack")) {

        QVariantMap params;
        for(const auto& key : json.object().keys()) {
            static const std::regex irrelevant{"what|status|type|protocol"};
            if (std::regex_search(key.toStdString(), irrelevant)) {
                continue;
            }
            params.insert(key, json.object().value(key).toString());
        }

        PeerAck ack{shared_from_this(), getConnectionId(), id,
                    object.value("what").toString().toUtf8(),
                    object.value("status").toString().toUtf8(),
                    params};

        if (ack.what.isEmpty() || ack.status.isEmpty()) {
            protocolError(QStringLiteral("Invalid acknowledgement packet"));
        }

        LFLOG_TRACE << "Emitting Ack";
        emit receivedAck(ack);
    } else if (type == QStringLiteral("Message")) {
        PeerMessage msg{shared_from_this(), getConnectionId(), id,
                    QByteArray::fromBase64(object.value("conversation").toString().toUtf8()),
                    QByteArray::fromBase64(object.value("message-id").toString().toUtf8()),
                    QDateTime::fromString(object.value("date").toString(), Qt::ISODate),
                    object.value("content").toString(),
                    QByteArray::fromBase64(object.value("from").toString().toUtf8()),
                    toEncoding(object.value("encoding").toString()),
                    QByteArray::fromBase64(object.value("signature").toString().toUtf8())};

        if (msg.data.conversation.isEmpty() || msg.data.messageId.isEmpty()) {
            protocolError(QStringLiteral("Invalid message packet"));
        }

        LFLOG_TRACE << "Emitting PeerMessage";
        emit receivedMessage(msg);
    } else if (type == QStringLiteral("IncomingFile")) {
        bool sizeOk = false;
        bool restOk = false;
        const auto size = object.value("size").toString().toLongLong(&sizeOk);
        const auto rest = object.value("rest").toString().toLongLong(&restOk);
        PeerFileOffer msg{shared_from_this(), getConnectionId(), id,
                    QByteArray::fromBase64(object.value("conversation").toString().toUtf8()),
                    QByteArray::fromBase64(object.value("file-id").toString().toUtf8()),
                    object.value("name").toString(),
                    size,
                    rest,
                    object.value("file-type").toString(),
                    QByteArray::fromBase64(object.value("sha256").toString().toUtf8())};

        if (msg.conversation.isEmpty() || msg.fileId.isEmpty()
                || msg.name.isEmpty() || msg.name.size() > 4096
                || !sizeOk || !restOk || msg.size < 0 || msg.rest < 0
                || msg.rest > msg.size || msg.sha512.size() != crypto_hash_sha256_BYTES) {
            protocolError(QStringLiteral("Invalid file offer packet"));
        }

        LFLOG_TRACE << "Emitting PeerFileOffer";
        emit receivedFileOffer(msg);
    } else if (type == QStringLiteral("SetAvatar")) {
        PeerSetAvatarReq avatar{shared_from_this(), getConnectionId(), id,
                    toQimage(object)};

        LFLOG_TRACE << "Emitting PeerSetAvatarReq";
        emit receivedAvatar(avatar);
    } else if (type == QStringLiteral("UserInfo")) {
        PeerUserInfo uinfo{shared_from_this(), getConnectionId(), id,
                    object.value("nick-name").toString(),
                    QByteArray::fromBase64(object.value("avatar-hash")
                                           .toString().toUtf8())};

        LFLOG_TRACE << "Emitting PeerUserInfo";
        emit receivedUserInfo(uinfo);
    } else {
        protocolError(QStringLiteral("Unknown control packet type"));
    }
}

void Peer::sendCapabilities()
{
    QJsonArray capabilities;
    capabilities.append(QStringLiteral("frame-v2"));
    capabilities.append(QStringLiteral("file-window-v1"));
    capabilities.append(QStringLiteral("keepalive-v1"));
    send(QJsonDocument{QJsonObject{
        {"type", "Capabilities"},
        {"capabilities", capabilities},
        {"max-frame", static_cast<int>(max_frame_bytes)}
    }});
}

[[noreturn]] void Peer::protocolError(const QString& reason)
{
    LFLOG_ERROR << "Protocol error on connection " << getConnectionId().toString()
                << ": " << reason;
    close();
    throw Error(reason);
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

    if (inState_ != InState::DISABLED) {
        throw runtime_error("Encrypted stream is already enabled");
    }
    resetIdleTimer();
    scheduleKeepAlive();
    wantChunkSize();
}

void Peer::resetIdleTimer()
{
    // Keepalive scheduling is independent of ordinary traffic so its
    // interval remains randomized.
}

void Peer::scheduleKeepAlive()
{
    if (inState_ != InState::CLOSING && inState_ != InState::DISABLED) {
        constexpr int minimumDelayMs = 90 * 1000;
        constexpr int maximumDelayMs = 240 * 1000;
        keepAliveTimer_.start(QRandomGenerator::system()->bounded(
            minimumDelayMs, maximumDelayMs + 1));
    }
}

void Peer::sendKeepAlive()
{
    if (inState_ == InState::CLOSING || !connection_->isOpen()) {
        return;
    }

    auto *random = QRandomGenerator::system();
    QByteArray nonce(16, Qt::Uninitialized);
    for (qsizetype offset = 0; offset < nonce.size(); offset += 4) {
        const auto value = random->generate();
        memcpy(nonce.data() + offset, &value,
               static_cast<size_t>(std::min<qsizetype>(4, nonce.size() - offset)));
    }
    const auto paddingSize = random->bounded(0, 257);
    QByteArray padding(paddingSize, Qt::Uninitialized);
    for (qsizetype offset = 0; offset < padding.size(); offset += 4) {
        const auto value = random->generate();
        memcpy(padding.data() + offset, &value,
               static_cast<size_t>(std::min<qsizetype>(4, padding.size() - offset)));
    }

    try {
        send(QJsonDocument{QJsonObject{
            {"type", "KeepAlive"},
            {"nonce", QString::fromLatin1(nonce.toBase64())},
            {"padding", QString::fromLatin1(padding.toBase64())}
        }});
    } catch (const std::exception& ex) {
        LFLOG_WARN << "Failed to send keepalive: " << ex.what();
    }
    scheduleKeepAlive();
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

    if (bytes < frame_header_bytes || bytes > max_frame_bytes) {
        throw runtime_error("Invalid encrypted frame length");
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
        if (final) {
            throw runtime_error("Unexpected final tag on encrypted frame length");
        }
        wantChunkData(qFromBigEndian(bytesToValue<quint16>(bytes)));
    } else if (inState_ == InState::CHUNK_DATA){

        static const QByteArray binary = {"[binary]"};
        if (ciphertext.size() < crypt_bytes + frame_header_bytes
                || ciphertext.size() > crypt_bytes + max_frame_bytes) {
            throw runtime_error("Invalid encrypted frame size");
        }
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

        if (buffer.size() != version.size() + channel.size()
                + id.size() + payload.size()) {
            throw runtime_error("Invalid encrypted frame layout");
        }

        decrypt(buffer_view, ciphertext, final);

        if (version.at(0) != static_cast<char>(frame_version)) {
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
    if (ciphertext.size() < crypt_bytes
            || data.size() + crypt_bytes != ciphertext.size()) {
        throw runtime_error("Invalid encrypted stream buffer size");
    }
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
        if (inChannels_.find(nextInchannel_) != inChannels_.end()) {
            throw Error("Incoming channel id collision");
        }
        channelId = nextInchannel_;
        inChannels_[channelId] = ch;
        ++nextInchannel_;
    } else {
        channelId = file.getChannel();
        if (channelId == 0 || outChannels_.find(channelId) != outChannels_.end()) {
            throw Error("Invalid or duplicate outgoing channel id");
        }
        ch = make_shared<OutgoingFileChannel>(filePtr);
        outChannels_[channelId] = ch;
    }

    const auto direction = file.getDirection();
    auto fileCPtr = filePtr.get();
    connect(fileCPtr, &File::stateChanged,
            this, [this, channelId, direction, fileCPtr]() {

        if (fileCPtr->getState() != File::FS_TRANSFERRING
                && !(direction == File::INCOMING
                     && fileCPtr->getState() == File::FS_HASHING)) {
            emit removeTransfer(direction, channelId);
            }
    });
    connect(fileCPtr, &File::transferDone,
            this, [this, channelId, direction](File *, bool success) {
        if (direction != File::INCOMING) {
            return;
        }
        const auto it = inChannels_.find(channelId);
        if (it != inChannels_.end()) {
            it->second->onTransferDone(*this, success);
        }
    });

    return channelId;
}

uint64_t Peer::startReceive(File &file)
{
    auto channelId = createChannel(file);
    const auto resumeOffset = inChannels_.at(channelId)->resumeOffset();

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
            {"rest", QString::number(resumeOffset)},
            {"data", QString{file.getFileId().toBase64()}},
            {"channel", channelId}
    };

    LFLOG_DEBUG << "Requesting File : " << file.getId()
                << " with channel #" << channelId
                << " over connection " << getConnectionId().toString();

    const auto rval = sendAck("IncomingFile", "Proceed", params);
    file.setBytesTransferred(resumeOffset);
    file.setState(File::FS_TRANSFERRING);
    file.setChannel(channelId);
    return rval;
}

uint64_t Peer::startSend(File &file)
{
    auto channelId = createChannel(file);
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
}

uint64_t Peer::sendUserInfo(const UserInfo &userInfo)
{
    auto json = QJsonDocument{
        QJsonObject{
            {"type", "UserInfo"},
            {"nick-name", userInfo.nickName},
            {"avatar-hash", QString{userInfo.avatarHash.toBase64()}}
        }
    };

    LFLOG_DEBUG << "Sending UserInfo over connection " << getConnectionId().toString();

    return send(json);
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
            {"rest", QString::number(file.getBytesTransferred())},
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
    if (file.getState() != File::FS_TRANSFERRING) {
        throw Error("File is not in transferring state");
    }

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

void Peer::acknowledgeFile(const QByteArray& fileId, quint64 sequence,
                           qlonglong offset, bool final)
{
    const auto file = DsEngine::instance().getFileManager()->getFileFromId(
        fileId, File::OUTGOING);
    if (file) {
        const auto it = outChannels_.find(file->getChannel());
        if (it != outChannels_.end()) {
            it->second->onAck(*this, sequence, offset, final);
            emit outputBufferEmptied();
            return;
        }
    }

    protocolError(QStringLiteral("Acknowledgement for unknown file transfer"));
}

void Peer::disableNotifications()
{
    notificationsDisabled_ = true;
}


}} // namespace
