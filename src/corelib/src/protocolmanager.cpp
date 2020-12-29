#include <memory>

#include "ds/protocolmanager.h"

using namespace std;

namespace ds {
namespace core {

std::map<ProtocolManager::Transport, ProtocolManager::factory_t> ProtocolManager::factories_;

ProtocolManager::ptr_t ProtocolManager::create(QSettings& settings, ProtocolManager::Transport transport)
{
    return factories_.at(transport)(settings);
}

void ProtocolManager::addFactory(ProtocolManager::Transport transport, ProtocolManager::factory_t factory)
{
    factories_[transport] = move(factory);
}


}} // namespaces

