#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>

#include "ds/connectionsocket.h"
#include "ds/peer.h"
#include "ds/dsclient.h"
#include "ds/dsserver.h"
#include "ds/handshake.h"
#include "ds/resourcelimits.h"
#include "ds/crypto.h"
#include "ds/message.h"
#include "ds/dscert.h"

class ProtocolV3Test final : public QObject
{
    Q_OBJECT

private slots:
    void recordConstants()
    {
        QCOMPARE(ds::prot::codec::protocolVersion, static_cast<quint32>(3));
        QCOMPARE(ds::prot::Peer::max_record_bytes, static_cast<size_t>(1'048'576));
        QCOMPARE(ds::prot::Peer::file_chunk_bytes, static_cast<size_t>(8192));
        QCOMPARE(ds::prot::Peer::file_window_chunks, static_cast<size_t>(8));
    }

    void checkpointWireFixture() {
        QFile file(QStringLiteral(DS_CHECKPOINT_WIRE_FIXTURES)+"/checkpoint-wire-v1.json");QVERIFY(file.open(QIODevice::ReadOnly));
        const auto expected=QJsonDocument::fromJson(file.readAll()).object();QCOMPARE(expected["version"].toInt(),1);
        ds::protocol::Envelope envelope;
        auto wire=QByteArray::fromHex(expected["envelopeHex"].toString().toLatin1());
        QVERIFY(ds::prot::codec::deserialize(wire,envelope));QVERIFY(envelope.hasFileCheckpoint());
        const auto &checkpoint=envelope.fileCheckpoint();
        QCOMPARE(checkpoint.fileId().toHex(),expected["fileId"].toString().toLatin1());
        QCOMPARE(checkpoint.attemptId().toHex(),expected["attemptId"].toString().toLatin1());
        QCOMPARE(checkpoint.checkpoint().nextOffset(),quint64(expected["offset"].toInt()));
        QVERIFY(ds::prot::codec::validateFileCheckpoint(checkpoint,131072,65536));
        QCOMPARE(ds::prot::codec::serialize(envelope),wire);
    }
    void checkpointNegotiationAndBounds() {
        using namespace ds::prot;
        auto capabilities=codec::makeCapabilities({"file-checkpoint-v1"},codec::maxRecordBytes,65536);
        QVERIFY(codec::validateCapabilities(capabilities));
        QCOMPARE(capabilities.capabilities().checkpointIntervalBytes(),quint32(65536));
        auto limits=capabilities.capabilities();limits.setCheckpointIntervalBytes(65537);capabilities.setCapabilities(limits);
        QVERIFY(!codec::validateCapabilities(capabilities));
        limits.setCheckpointIntervalBytes(65536);limits.setCapabilities({"file-chunks-v3"});capabilities.setCapabilities(limits);
        QVERIFY(!codec::validateCapabilities(capabilities));
        ds::protocol::CheckpointParameters parameters;parameters.setVersion(1);parameters.setIntervalBytes(65536);
        parameters.setAttemptId(QByteArray(16,'a'));parameters.setNextCheckpointSequence(1);
        QVERIFY(codec::validateCheckpointParameters(parameters));
        parameters.setVersion(2);QVERIFY(!codec::validateCheckpointParameters(parameters));
        ds::protocol::CheckpointReference reference;reference.setSequence(1);reference.setNextOffset(65536);reference.setPrefixSha256(QByteArray(32,'d'));
        QVERIFY(codec::validateCheckpointReference(reference,131072,65536));
        reference.setNextOffset(65537);QVERIFY(!codec::validateCheckpointReference(reference,131072,65536));
        reference.setPauseBoundary(true);QVERIFY(codec::validateCheckpointReference(reference,131072,65536));
        reference.setFinal(true);QVERIFY(!codec::validateCheckpointReference(reference,131072,65536));
        reference.setNextOffset(131072);QVERIFY(codec::validateCheckpointReference(reference,131072,65536));
        ds::protocol::FileCheckpoint checkpoint;checkpoint.setFileId(QByteArray(16,'f'));checkpoint.setAttemptId(QByteArray(16,'a'));
        checkpoint.setCheckpoint(reference);checkpoint.setExpectedSha256(QByteArray(32,'e'));
        checkpoint.setPhase(ds::protocol::FileCheckpoint::Phase::CHECKPOINT_PROPOSED);
        QVERIFY(codec::validateFileCheckpoint(checkpoint,131072,65536));
        checkpoint.setPhase(ds::protocol::FileCheckpoint::Phase(99));QVERIFY(!codec::validateFileCheckpoint(checkpoint,131072,65536));
    }
    void resumeCheckpointSetsAreBounded() {
        using namespace ds::prot;
        ds::protocol::FileResume resume;resume.setFileId(QByteArray(16,'f'));resume.setFileSize(131072);
        resume.setSha256(QByteArray(32,'h'));resume.setCheckpointVersion(1);resume.setCheckpointIntervalBytes(65536);
        ds::protocol::CheckpointReference zero;zero.setPrefixSha256(QCryptographicHash::hash(QByteArray{},QCryptographicHash::Sha256));
        resume.setAvailableCheckpoints({zero});QVERIFY(codec::validateResumeCheckpoints(resume));
        resume.setAvailableCheckpoints({zero,zero});QVERIFY(!codec::validateResumeCheckpoints(resume));
        ds::protocol::CheckpointReference prefix;prefix.setSequence(1);prefix.setNextOffset(65536);prefix.setPrefixSha256(QByteArray(32,'p'));
        resume.setAvailableCheckpoints({prefix});QVERIFY(!codec::validateResumeCheckpoints(resume));
        resume.setAvailableCheckpoints({zero,prefix});QVERIFY(codec::validateResumeCheckpoints(resume));
        auto too_many=resume.availableCheckpoints();while(too_many.size()<65)too_many.append(prefix);
        resume.setAvailableCheckpoints(too_many);QVERIFY(!codec::validateResumeCheckpoints(resume));
        resume.setAvailableCheckpoints({zero});resume.setCheckpointVersion(2);QVERIFY(!codec::validateResumeCheckpoints(resume));
    }
    void outputQueueIsBounded()
    {
        ds::prot::ConnectionSocket socket;
        const QByteArray frame(static_cast<int>(
                                    ds::prot::ConnectionSocket::maxOutgoingBytes()), '\0');
        socket.write(frame);
        QCOMPARE(socket.queuedBytes(),
                 ds::prot::ConnectionSocket::maxOutgoingBytes());
        QVERIFY_EXCEPTION_THROWN(socket.write(QByteArray(1, '\0')), std::runtime_error);
    }

    void handshakeTimeoutsAreBounded()
    {
        QVERIFY(ds::prot::DsClient::handshakeTimeoutMilliseconds > 0);
        QVERIFY(ds::prot::DsClient::handshakeTimeoutMilliseconds <= 30'000);
        QVERIFY(ds::prot::DsServer::handshakeTimeoutMilliseconds > 0);
        QVERIFY(ds::prot::DsServer::authorizationTimeoutMilliseconds
                > ds::prot::DsServer::handshakeTimeoutMilliseconds);
    }

    void handshakeSizesAreFixed()
    {
        QCOMPARE(ds::prot::handshake::clientHelloBytes, static_cast<size_t>(161));
        QCOMPARE(ds::prot::handshake::serverHelloBytes, static_cast<size_t>(153));
        QCOMPARE(ds::prot::handshake::clientFinishBytes, static_cast<size_t>(121));
    }

    void resourceLimitsAreClampedToV3Ceilings()
    {
        QSettings settings;
        settings.clear();
        settings.setValue("resourceAcceptedFileBytes", qulonglong{1});
        settings.setValue("resourceSocketOutputBytes", 999999999);
        settings.setValue("resourceQueuedFileOffers", 999999999);

        const auto limits = ds::core::ResourceLimits::fromSettings(settings);
        QCOMPARE(limits.acceptedFileBytes,
                 ds::core::ResourceLimits::defaultAcceptedFileBytes);
        QCOMPARE(limits.socketOutputBytes,
                 ds::core::ResourceLimits::hardSocketOutputBytes);
        QCOMPARE(limits.queuedFileOffers,
                 ds::core::ResourceLimits::hardQueuedFileOffers);
        QCOMPARE(limits.incomingDiskBytes,
                 ds::core::ResourceLimits::defaultIncomingDiskBytes);
        QCOMPARE(limits.minimumFreeSpacePercent,
                 ds::core::ResourceLimits::defaultMinimumFreeSpacePercent);
    }

    void signedAttachmentRelationshipsAreAuthenticatedAndBounded()
    {
        ds::crypto::Crypto crypto;
        const auto cert = ds::crypto::DsCert::create();
        ds::core::MessageData data;
        data.conversation = QByteArray(32, 'c'); data.messageId = QByteArray(32, 'm');
        data.composedTime = QDateTime::fromSecsSinceEpoch(
            (QDateTime::currentSecsSinceEpoch() / 60) * 60, QTimeZone::UTC);
        data.encoding = ds::core::Message::US_ACSII;
        data.attachments = {
            {QByteArray(16, 'a'), "photo.png", 10000, QByteArray(32, 'h'), "image/png", {}},
            {QByteArray(16, 'b'), "preview.jpg", 2048, QByteArray(32, 't'), "image/jpeg", quint32{0}}};
        QObject parent;
        ds::core::Message message(parent, data, ds::core::Message::OUTGOING, 1);
        message.sign(*cert); QVERIFY(message.validate(*cert));
        const auto signed_data = message.getData();
        for (int field = 0; field < 5; ++field) {
            auto changed = signed_data;
            switch (field) {
            case 0: changed.attachments[0].name = "other.png"; break;
            case 1: changed.attachments[0].size++; break;
            case 2: changed.attachments[0].sha256[0] ^= 1; break;
            case 3: changed.attachments[0].mime_type = "image/jpeg"; break;
            case 4: changed.attachments[1].thumbnail_for.reset(); break;
            }
            ds::core::Message tampered(parent, changed, ds::core::Message::INCOMING, 1);
            QVERIFY(!tampered.validate(*cert));
        }
        // Relays may replace transport-local file IDs, while preserving all
        // signed attachment metadata and the relationship.
        auto relayed = signed_data; relayed.attachments[0].file_id = QByteArray(16, 'r');
        ds::core::Message relay(parent, relayed, ds::core::Message::OUTGOING, 1);
        QVERIFY(relay.validate(*cert));
        for (int invalid = 0; invalid < 4; ++invalid) {
            auto changed = signed_data;
            if (invalid == 0) changed.attachments[1].thumbnail_for = 2;
            if (invalid == 1) changed.attachments[0].thumbnail_for = 1;
            if (invalid == 2) changed.attachments[1].file_id = changed.attachments[0].file_id;
            if (invalid == 3) changed.attachments.resize(17, changed.attachments.front());
            ds::core::Message unsupported(parent, changed, ds::core::Message::OUTGOING, 1);
            QVERIFY(!unsupported.validate(*cert));
        }
    }
    void messageValidationRejectsInvalidEncodingContentAndTime()
    {
        ds::crypto::Crypto crypto;
        const auto cert = ds::crypto::DsCert::create();
        const auto now = QDateTime::currentDateTimeUtc();
        ds::core::MessageData data;
        data.conversation = QByteArray(32, 'c');
        data.messageId = QByteArray(32, 'm');
        data.composedTime = QDateTime::fromSecsSinceEpoch(
            (now.toSecsSinceEpoch() / 60) * 60, QTimeZone::UTC);
        data.content = QStringLiteral("hello");
        data.sender = cert->getSigningPubKey().toByteArray();
        data.encoding = ds::core::Message::UTF8;

        QObject parent;
        ds::core::Message message(parent, data, ds::core::Message::OUTGOING, 1);
        message.sign(*cert);
        QVERIFY(message.validate(*cert));

        data.encoding = ds::core::Message::US_ACSII;
        data.content = QString::fromUtf8("bad\x01");
        ds::core::Message invalidContent(parent, data, ds::core::Message::OUTGOING, 1);
        invalidContent.sign(*cert);
        QVERIFY(!invalidContent.validate(*cert));
    }
};

QTEST_MAIN(ProtocolV3Test)
#include "protocol_v3_test.moc"
