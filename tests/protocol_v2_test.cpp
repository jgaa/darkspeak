#include <QtTest>

#include "ds/connectionsocket.h"
#include "ds/peer.h"

class ProtocolV2Test final : public QObject
{
    Q_OBJECT

private slots:
    void frameConstants()
    {
        QCOMPARE(ds::prot::Peer::frame_version, static_cast<quint8>(2));
        QCOMPARE(ds::prot::Peer::control_version, static_cast<quint8>(2));
        QCOMPARE(ds::prot::Peer::frame_header_bytes, static_cast<size_t>(13));
        QCOMPARE(ds::prot::Peer::file_chunk_header_bytes, static_cast<size_t>(16));
        QCOMPARE(ds::prot::Peer::file_chunk_bytes, static_cast<size_t>(8192));
        QCOMPARE(ds::prot::Peer::file_window_chunks, static_cast<size_t>(8));
        QCOMPARE(ds::prot::Peer::file_retry_limit, 5);
        QCOMPARE(ds::prot::Peer::max_payload_bytes,
                 ds::prot::Peer::max_frame_bytes - ds::prot::Peer::frame_header_bytes);
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
};

QTEST_MAIN(ProtocolV2Test)
#include "protocol_v2_test.moc"
