#include "include/ds/connectionsocket.h"

namespace ds {
namespace prot {

ConnectionSocket::ConnectionSocket()
{
    connect(this, &ConnectionSocket::connected,
            this, &ConnectionSocket::onConnected);
    connect(this, &ConnectionSocket::disconnected,
            this, &ConnectionSocket::onDisconnected);

    // error() is ambigous
    connect(this, SIGNAL(error(SocketError)),
            this, SLOT(onSocketFailed(SocketError)));
}

void ConnectionSocket::onConnected()
{
    emit connectedToHost(uuid);
}

void ConnectionSocket::onDisconnected()
{
    emit disconnectedFromHost(uuid);
}

void ConnectionSocket::onSocketFailed(const SocketError& socketError)
{
    emit socketFailed(uuid, socketError);
}

}}
