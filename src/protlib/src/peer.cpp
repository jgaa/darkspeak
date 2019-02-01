#include <array>
#include <vector>
#include <sodium.h>
#include "ds/peer.h"
#include "logfault/logfault.h"

namespace ds {
namespace prot {

using namespace  std;

Peer::Peer(ConnectionSocket::ptr_t connection,
                   core::ConnectData connectionData)
    : connection_{move(connection)}, connectionData_{move(connectionData)}
{
}

}} // namespace

