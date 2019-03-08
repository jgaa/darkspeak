#ifndef DSSERVER_H
#define DSSERVER_H

#include "ds/peer.h"


namespace ds {
namespace prot {

class DsServer : public Peer
{
    Q_OBJECT
public:
    enum class State {
        CONNECTED,
        WAITING_FOR_AUTHORIZATION,
        ENCRYPTED_STREAM,
        FAILED,
        UNAUTHORIZED
    };

    DsServer(ConnectionSocket::ptr_t connection, core::ConnectData connectionData);

public slots:
    virtual void authorize(bool authorize) override;

private slots:
    void advance(const data_t& data);

private:
    void getHello(const data_t& data);

    State state_ = State::CONNECTED;

    // PeerConnection interface
public:
    Direction getDirection() const noexcept override {
        return INCOMING;
    }
};

}} // namespaces

#endif // DSSERVER_H
