
#include <assert.h>

#include <boost/property_tree/info_parser.hpp>

#include "darkspeak/darkspeak_impl.h"
#include "darkspeak/ImManager.h"


using namespace std;
using namespace war;

namespace darkspeak {
namespace impl {

ImManager::ImManager(path_t conf_file)
{
    // Read the configuration
    if (boost::filesystem::exists(conf_file)) {
        boost::property_tree::read_info(conf_file.string(), config_);
    } else {
        LOG_ERROR << "File " << log::Esc(conf_file.string()) << " don't exist.";
        WAR_THROW_T(ExceptionNotFound, conf_file.string());
    }
}

ImManager::~ImManager()
{

}

Api::Buddy::ptr_t ImManager::AddBuddy(Buddy::Info def)
{
    assert(false && "Not implemented");
}


void ImManager::Disconnect(bool local_only)
{
    assert(false && "Not implemented");
}

std::vector< Api::buddy_list_t > ImManager::GetBuddies()
{
    assert(false && "Not implemented");
}

void ImManager::GoOnline()
{
    assert(false && "Not implemented");
}

void ImManager::Panic(std::__cxx11::string message, bool erase_data)
{
    assert(false && "Not implemented");
}


} // impl
} // darkspeak
