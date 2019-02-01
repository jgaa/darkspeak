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
        CONNECTED
    };

    DsServer(ConnectionSocket::ptr_t connection, core::ConnectData connectionData);

private slots:
    void advance();

private:
    void getHello();

    State state_ = State::CONNECTED;
};

}} // namespaces

#endif // DSSERVER_H
