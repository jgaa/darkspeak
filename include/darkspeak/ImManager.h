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
#include "darkspeak/Config.h"

#ifdef SendMessage
// Thank you SO much Micro$oft!
#	undef SendMessage
#endif

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
        void OnBuddyDeleted(const DeletedBuddyInfo& info) override {};
        void OnBuddyStateUpdate(const BuddyInfo& info) override;
        void OnIncomingMessage(const Message& message) override;
        void OnIncomingFile(const FileInfo& file) override;
        void OnFileTransferUpdate(const FileInfo& file) override;
        void OnOtherEvent(const Event& event) override;
        void OnListening(const ListeningInfo& endpoint) override;
        void OnShutdownComplete(const ShutdownInfo& info) override;
        void OnAvatarReceived(const AvatarInfo& info) override;
    private:
        ImManager& manager_;
    };

    ~ImManager();

    buddy_list_t GetBuddies() override;
    Buddy::ptr_t AddBuddy(const Buddy::Info& def) override;
    Buddy::ptr_t GetBuddy(const std::string& id)override;
    void RemoveBuddy(const std::string& id) override;
    void GoOnline() override;
    void Disconnect(bool local_only = true) override;
    void Panic(std::string message, bool erase_data) override;
    void SetMonitor(std::shared_ptr<EventMonitor> monitor) override;
    void AcceptFileTransfer(const AcceptFileTransferData& aftd) override;
    void RejectFileTransfer(const AcceptFileTransferData& aftd) override;
    void AbortFileTransfer(const AbortFileTransferData& aftd) override;


    std::shared_ptr<ImProtocol> GetProtocol() { return protocol_; }
    void Connect(Api::Buddy::ptr_t buddy);
    std::shared_ptr<Config> GetConfig() const {
        return config_;
    }

    // Returne nullpointer if not found
    BuddyImpl::ptr_t GetBuddyImpl(const std::string& id);

    bool HaveBuddy(const std::string& id) const;

    std::vector<std::shared_ptr<EventMonitor>> GetMonitors();

    war::Threadpool& GetThreadpool() { return *threadpool_; }

    static std::shared_ptr<ImManager> CreateInstance(path_t conf_file);

    Info GetInfo();

protected:
    void Init();
    ImManager(path_t conf_file);

private:
    void LoadBuddies();
    void SaveBuddies();

    mutable std::mutex mutex_;
    std::shared_ptr<Config> config_;
    std::map<std::string, BuddyImpl::ptr_t> buddies_;
    std::shared_ptr<ImProtocol> protocol_;
    std::unique_ptr<war::Threadpool> threadpool_;
    std::vector<std::weak_ptr<EventMonitor>> event_monitors_;
    std::shared_ptr<Events> event_monitor_;
};

} // impl
} // darkspeak
