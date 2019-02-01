#include "include/ds/dsserver.h"

#include "logfault/logfault.h"

namespace ds {
namespace prot {

using namespace  std;

DsServer::DsServer(ds::prot::ConnectionSocket::ptr_t
                             connection, ds::core::ConnectData connectionData)
    : Peer{move(connection), move(connectionData)}
{
    advance();
}

void DsServer::advance()
{
    switch(state_) {
        case State::CONNECTED:
            LFLOG_DEBUG << "Peer connected from " << connectionData_.address
                        << " with id " << connection_->getUuid().toString()
                        << ". Starting DS protocol.";
            getHello();
            break;
    }
}

void DsServer::getHello()
{
    if (!connection_->isReadable()) {
        return;
    }

    Hello hello;

    // Read exactely into hello.

    // Unencrypt the payload

    // Check version

    // Validate the signature

    // Prepare outbound encryption

    // Send a signal to authorize the connection (it may be blacklisted)

    // Stall further IO until we get directions
}

}} // namespaces
