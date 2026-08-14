#ifndef DSCLIENT_H
#define DSCLIENT_H

#include "ds/peer.h"
#include "ds/handshake.h"
#include <QTimer>
#include <array>

namespace ds {
namespace prot {

/*! Client implementation of the DarkSpeak protocol
 *
 */
class DsClient : public Peer
{
    Q_OBJECT
public:
    static constexpr int handshakeTimeoutMilliseconds = 15'000;

    using ptr_t = std::shared_ptr<DsClient>;

    enum class State {
        CONNECTED,
        GET_SERVER_HELLO,
        ENCRYPTED_STREAM
    };

    DsClient(ConnectionSocket::ptr_t connection, core::ConnectData connectionData);
    ~DsClient() override = default;

private slots:
    void advance();
    void advance(const data_t& data);


private:
    void sayHello();
    void getServerHello(const data_t& data);
    void startConnectRetryTimer();
    void initConnections();
    void failHandshake() noexcept;

    State state_ = State::CONNECTED;
    size_t maxReconnects_ = 20;
    size_t numReconnects_ = {};
    size_t reconnectDelayMilliseconds_ = 20000;
    QTimer handshakeTimer_;
    handshake::BudgetLease handshakeBudget_;
    QByteArray clientHello_;
    std::array<unsigned char, crypto_kx_PUBLICKEYBYTES> ephemeralPublicKey_{};
    std::array<unsigned char, crypto_kx_SECRETKEYBYTES> ephemeralSecretKey_{};
    std::array<unsigned char, 32> clientNonce_{};
    std::array<unsigned char, crypto_kx_SESSIONKEYBYTES> clientTx_{};

    // PeerConnection interface
public:
    Direction getDirection() const noexcept override {
        return OUTGOING;
    }
};

}} // namespaces

#endif // DSCLIENT_H
