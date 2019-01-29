
#include "include/ds/dsclient.h"

namespace ds {
namespace prot {

using namespace  std;

DsClient::DsClient(ConnectionSocket::ptr_t connection)
    : connection_{move(connection)}
{
    // TODO: Set timeout
    advance();
}

void DsClient::advance()
{
    switch(state_) {
        case State::CONNECTED:
            sayHello();
        break;
    }
}

/* Create an initial message to the server
 *
 * The message aims to validate that the server is the expected
 * receipient before revealing our own identity.
 *
 * The server will add a 16 byte nounce, sign the nounce + hash, encrypt the
 * data using the hash as pasword and reply back to us. Then we will send the
 * server our signed pubkey, encrypted using the hash as password.
 */
void DsClient::sayHello()
{
    if (!connection_->isWritable()) {
        return;
    }

    // Create a hash of the servers pubkey, a timestamp and 16 random bytes.

    // Compose a message: version_byte + hash + timestamp + random bytes

    // Encrypt the message with the servers pubkey

    // Send the message to the server.
}



}} // namespaces
