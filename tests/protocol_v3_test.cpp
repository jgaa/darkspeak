#include <QtTest>

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
