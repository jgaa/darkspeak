#include <assert.h>
#include "ds/tormgr.h"

namespace ds {
namespace tor {


TorMgr::TorMgr(const Config& config, QObject *parent)
    : QObject(parent), config_{config}
{

}

void TorMgr::start()
{
    if (config_.mode == Config::Mode::SYSTEM) {
        startUseSystemInstance();
    } else {
        assert(false); // Not implemented
    }
}

void TorMgr::startUseSystemInstance()
{

}

}} // namespaces
