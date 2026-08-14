#ifndef DSSERVER_H
#define DSSERVER_H

#include "ds/peer.h"
#include "ds/handshake.h"
#include <QTimer>
#include <array>


namespace ds {
namespace prot {

class DsServer : public Peer
{
    Q_OBJECT
public:
    static constexpr int handshakeTimeoutMilliseconds = 15'000;
    static constexpr int authorizationTimeoutMilliseconds = 30'000;

    enum class State {
        CONNECTED,
        WAITING_FOR_FINISH,
        WAITING_FOR_AUTHORIZATION,
        ENCRYPTED_STREAM,
        FAILED,
        UNAUTHORIZED
    };

    DsServer(ConnectionSocket::ptr_t connection, core::ConnectData connectionData);
    ~DsServer() override = default;

public slots:
    virtual void authorize(bool authorize) override;

private slots:
    void advance(const data_t& data);

private:
    void getHello(const data_t& data);
    void getClientFinish(const data_t& data);
    void failHandshake() noexcept;

    State state_ = State::CONNECTED;
    QTimer handshakeTimer_;
    handshake::BudgetLease handshakeBudget_;
    QByteArray clientHello_;
    QByteArray serverHello_;
    QByteArray clientSigningPublicKey_;
    std::array<unsigned char, crypto_kx_PUBLICKEYBYTES> ephemeralPublicKey_{};
    std::array<unsigned char, crypto_kx_SECRETKEYBYTES> ephemeralSecretKey_{};
    std::array<unsigned char, 32> serverNonce_{};
    std::array<unsigned char, crypto_kx_SESSIONKEYBYTES> serverRx_{};
    std::array<unsigned char, crypto_kx_SESSIONKEYBYTES> serverTx_{};

    // PeerConnection interface
public:
    Direction getDirection() const noexcept override {
        return INCOMING;
    }
};

}} // namespaces

#endif // DSSERVER_H
