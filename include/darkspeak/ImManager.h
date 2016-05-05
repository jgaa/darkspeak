#pragma once

#include <string>
#include <memory>
#include <boost/asio.hpp>
#include <boost/asio/yield.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>

#include "darkspeak/Api.h"

namespace darkspeak {
namespace impl {


/*! Real implementation of the Api interface.
 *
 * This implementation owns the data and the
 * hidden service(s).
 */
class ImManager : public Api
{
public:
    ImManager(path_t conf_file);
    ~ImManager();

    std::vector< buddy_list_t > GetBuddies() override;
    Buddy::ptr_t AddBuddy(Buddy::Info def) override;
    void GoOnline() override;
    void Disconnect(bool local_only = true) override;
    void Panic(std::string message, bool erase_data) override;

private:
    boost::property_tree::ptree config_;
};

} // impl
} // darkspeak
