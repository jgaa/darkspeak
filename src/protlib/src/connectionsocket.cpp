
#include "include/ds/connectionsocket.h"
#include "logfault/logfault.h"

using namespace std;

namespace ds {
namespace prot {

ConnectionSocket::ConnectionSocket(QByteArray host,
                                   quint16 port, const QUuid &uuid)
    : host_{move(host)}, port_{port}
{
    setReadBufferSize(1024 * 64);

    if (uuid.isNull()) {
        this->uuid = QUuid::createUuid();
    } else {
        this->uuid = uuid;
    }

    connect(this, &ConnectionSocket::connected,
            this, &ConnectionSocket::onConnected);
    connect(this, &ConnectionSocket::disconnected,
            this, &ConnectionSocket::onDisconnected);

//    // error() is ambigous
//    connect(this, SIGNAL(error(SocketError)),
//            this, SLOT(onSocketFailed(SocketError)));

    connect(this, &ConnectionSocket::readyRead, this, [this]() {
        inData += readAll();
        processInput();
    });

    connect(this, &ConnectionSocket::bytesWritten,
            this, [this](qint64 bytes) {

        Q_UNUSED(bytes)

        if (outData.isEmpty()) {
            emit outputBufferEmptied();
        } else {
            sendMore();
        }
    });

    LFLOG_TRACE << "Socket is constructed: " << uuid.toString();
}

ConnectionSocket::~ConnectionSocket()
{
    LFLOG_TRACE << "Socket is destructed: " << uuid.toString();
}

void ConnectionSocket::wantBytes(size_t bytesRequested)
{
    bytesWanted_ = bytesRequested;
    processInput();
}

void ConnectionSocket::connectToDefaultHost()
{
    connectToHost(host_, port_);
}

void ConnectionSocket::onConnected()
{
    LFLOG_DEBUG << "Socket on connection " << uuid.toString()
                << " is connected.";
    emit connectedToHost(uuid);
}

void ConnectionSocket::onDisconnected()
{
    LFLOG_DEBUG << "Socket on connection " << uuid.toString()
                << " was disconnected.";

    emit disconnectedFromHost(uuid);
}

void ConnectionSocket::onSocketFailed(SocketError socketError)
{
    LFLOG_DEBUG << "Socket on connection " << uuid.toString()
                << " was failed with error: "
                << socketError;

    emit socketFailed(uuid, socketError);
}

void ConnectionSocket::processInput()
{
    if (bytesWanted_ && (static_cast<size_t>(inData.size()) >= bytesWanted_)) {
        const data_t data{inData.data(), bytesWanted_};

        QByteArray my_data;
        if (bytesWanted_ == static_cast<size_t>(inData.size())) {
            my_data.swap(inData);
            assert(inData.isEmpty());
        } else {
            my_data = inData.left(static_cast<int>(bytesWanted_));
            inData.remove(0, static_cast<int>(bytesWanted_));
        }

        // We may be called recursively, so InData must be updated before we emit
        bytesWanted_ = 0;
        emit haveBytes(my_data);
    }

    // This should never happen, but just in case...
    if (static_cast<size_t>(inData.size()) > maxInDataSize) {
        LFLOG_ERROR << "To much data ("
                   << inData.size()
                   << ") in incoming buffer on " << getUuid().toString();
        close();
    }
}

void ConnectionSocket::sendMore()
{
    if (outData.isEmpty()) {
        return;
    }

    auto written = QTcpSocket::write(outData);
    if (written > 0) {
        if (written == outData.size()) {
            outData.clear();
        } else {
            outData.remove(0, static_cast<int>(written));
        }
    }
}

}}
