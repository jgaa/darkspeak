#include <assert.h>
#include "ds/tormgr.h"

namespace ds {
namespace tor {


TorMgr::TorMgr(const TorConfig& config, QObject *parent)
    : QObject(parent), config_{config}
{

}

/* To start using a tor service, we need to connect over TCP to
 * the contol channel of a running server.
 * Then we need to get the authentication methods from the
 * PROTOCOLINFO command, and based on that,
 * perform one of the following authentication operations
 * (listed in the preffered order):
 *  - PASSWORD
 *  - SAFECOOKIE
 *  - COOKIE (depricated)
 *
 * We don't support NULL. Too suspicious!
 */

void TorMgr::start()
{
    if (config_.mode == TorConfig::Mode::SYSTEM) {
        startUseSystemInstance();
    } else {
        assert(false); // Not implemented
    }
}

void TorMgr::startUseSystemInstance()
{

}

}} // namespaces
