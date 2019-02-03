#include "include/ds/connectionsocket.h"
#include "logfault/logfault.h"

namespace ds {
namespace prot {

ConnectionSocket::ConnectionSocket()
{
    setReadBufferSize(1024 * 64);

    connect(this, &ConnectionSocket::connected,
            this, &ConnectionSocket::onConnected);
    connect(this, &ConnectionSocket::disconnected,
            this, &ConnectionSocket::onDisconnected);

    // error() is ambigous
    connect(this, SIGNAL(error(SocketError)),
            this, SLOT(onSocketFailed(SocketError)));

    connect(this, &ConnectionSocket::readyRead, this, [this]() {
        inData += readAll();
        processInput();
    });
}

void ConnectionSocket::wantBytes(size_t bytesRequested)
{
    bytesWanted_ = bytesRequested;
    processInput();
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

void ConnectionSocket::processInput()
{
    if (bytesWanted_ && (static_cast<size_t>(inData.size()) >= bytesWanted_)) {
        const data_t data{inData.data(), bytesWanted_};
        emit haveBytes(data);

        if (static_cast<size_t>(inData.size()) == bytesWanted_) {
            inData.resize(0);
        } else {
            inData.remove(0, static_cast<int>(bytesWanted_));
        }

        bytesWanted_ = 0;
    }

    // This should never happen, but just in case...
    if (static_cast<size_t>(inData.size()) > maxInDataSize) {
        LFLOG_ERROR << "To much data ("
                   << inData.size()
                   << ") in incoming buffer on " << getUuid().toString();
        close();
    }
}

}}
