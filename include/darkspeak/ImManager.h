#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <map>
#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>

#include "tasks/WarThreadpool.h"

#include "darkspeak/Api.h"

namespace darkspeak {

class ImProtocol;

namespace impl {

/*! Real implementation of the Api interface.
 *
 * This implementation owns the data and the
 * hidden service(s).
 */
class ImManager : public Api,
    public std::enable_shared_from_this<ImManager>
{
public:
    ImManager(path_t conf_file);
    ~ImManager();

    buddy_list_t GetBuddies() override;
    Buddy::ptr_t AddBuddy(const Buddy::Info& def) override;
    void RemoveBuddy(const std::string& id) override;
    void GoOnline(const Info& my_info) override;
    void Disconnect(bool local_only = true) override;
    void Panic(std::string message, bool erase_data) override;

    std::shared_ptr<ImProtocol> GetProtocol() { return protocol_; }
    void Connect(Api::Buddy::ptr_t buddy);
    boost::property_tree::ptree GetConfig() const {
        return config_;
    }

private:
    void LoadBuddies();
    void SaveBuddies();

    mutable std::mutex mutex_;
    boost::property_tree::ptree config_;
    // id, pointer
    std::map<std::string, Buddy::ptr_t> buddies_;
    std::shared_ptr<ImProtocol> protocol_;
    std::unique_ptr<war::Threadpool> threadpool_;
    Info my_info_;
};

} // impl
} // darkspeak
