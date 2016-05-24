#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <map>
#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>

#include "tasks/WarThreadpool.h"

#include "darkspeak/Api.h"
#include "darkspeak/EventMonitor.h"
#include "darkspeak/BuddyImpl.h"

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
    class Events : public EventMonitor {
    public:
        Events(ImManager& manager)
        : manager_{manager} {}

        bool OnIncomingConnection(const ConnectionInfo& info) override;
        bool OnAddNewBuddy(const BuddyInfo& info) override;
        void OnNewBuddyAdded(const BuddyInfo& info) override;
        void OnBuddyStateUpdate(const BuddyInfo& info) override;
        void OnIncomingMessage(const Message& message) override;
        void OnIncomingFile(const FileInfo& file) override;
        void OnOtherEvent(const Event& event) override;
        void OnListening(const ListeningInfo& endpoint) override;
        void OnShutdownComplete(const ShutdownInfo& info) override;

    private:
        ImManager& manager_;
    };

    ~ImManager();

    buddy_list_t GetBuddies() override;
    Buddy::ptr_t AddBuddy(const Buddy::Info& def) override;
    void RemoveBuddy(const std::string& id) override;
    void GoOnline(const Info& my_info) override;
    void Disconnect(bool local_only = true) override;
    void Panic(std::string message, bool erase_data) override;
    void SetMonitor(std::shared_ptr<EventMonitor> monitor) override;

    std::shared_ptr<ImProtocol> GetProtocol() { return protocol_; }
    void Connect(Api::Buddy::ptr_t buddy);
    boost::property_tree::ptree GetConfig() const {
        return config_;
    }

    // Returne nullpointer if not found
    BuddyImpl::ptr_t GetBuddy(const std::string& id);

    bool HaveBuddy(const std::string& id) const;

    template <typename T>
    T GetConfigValue(std::string key, const T& def) {
        return config_.get(key, def);
    }

    template <typename T = std::string>
    T GetConfigValue(std::string key) {
        return config_.get<T>(key);
    }

    std::vector<std::shared_ptr<EventMonitor>> GetMonitors();

    war::Threadpool& GetThreadpool() { return *threadpool_; }

    static std::shared_ptr<ImManager> CreateInstance(path_t conf_file);

protected:
    void Init();
    ImManager(path_t conf_file);

private:
    void LoadBuddies();
    void SaveBuddies();

    mutable std::mutex mutex_;
    boost::property_tree::ptree config_;
    // id, pointer
    std::map<std::string, BuddyImpl::ptr_t> buddies_;
    std::shared_ptr<ImProtocol> protocol_;
    std::unique_ptr<war::Threadpool> threadpool_;
    std::vector<std::weak_ptr<EventMonitor>> event_monitors_;
    Info my_info_;
    std::shared_ptr<Events> event_monitor_;
    path_t conf_file_;
};

} // impl
} // darkspeak
