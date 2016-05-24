
#include <assert.h>

#include <boost/property_tree/info_parser.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/utility.hpp>
#include <boost/serialization/list.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/version.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>

#include "war_helper.h"

#include "darkspeak/darkspeak_impl.h"
#include "darkspeak/ImManager.h"
#include "darkspeak/ImProtocol.h"
#include "darkspeak/BuddyImpl.h"
#include "darkspeak/weak_container.h"

using namespace std;
using namespace war;

std::ostream& operator << (std::ostream& o, const darkspeak::Direction& v) {
    static const array<string, 2> names = { "INCOMING", "OUTGOING" };

    return o << names.at(static_cast<int>(v));
}

std::ostream& operator << (std::ostream& o, const darkspeak::Api::Status& v) {
    static const array<string, 5> names
        = { "OFF_LINE", "AVAILABLE", "BUSY", "AWAY", "LONG_TIME_AWAY" };

    return o << names.at(static_cast<int>(v));
}

std::ostream& operator << (std::ostream& o, const darkspeak::Api::Presence& v) {
    static const array<string, 3> names = { "OFF_LINE", "CONNECTING", "ON_LINE" };

    return o << names.at(static_cast<int>(v));
}


#define LOCK std::lock_guard<std::mutex> lock(mutex_);

namespace darkspeak {
namespace impl {

struct InfoCapsule : public Api::Buddy::Info {

    InfoCapsule() = default;

    InfoCapsule(Api::Buddy::Info&& v)
    : Api::Buddy::Info{move(v)} {}

    template<class Archive>
    void serialize(Archive & ar, auto version)
    {
        ar & id;
        ar & profile_name;
        ar & profile_text;
        ar & our_nickname;
        ar & our_general_notes;
        ar & created_time;
        ar & first_contact;
        ar & last_seen;
        ar & anonymity;
        ar & required_anonymity;
        ar & auto_connect;
        ar & store_conversations;
    }
};

struct InfoData {
    std::vector<InfoCapsule> data;

    template<class Archive>
    void serialize(Archive & ar, auto version)
    {
        ar & data;
    }
};

ImManager::ImManager(path_t conf_file)
: conf_file_{conf_file}
{
}

ImManager::~ImManager()
{
    SaveBuddies();
}

void ImManager::Init()
{
    // Read the configuration
    if (boost::filesystem::exists(conf_file_)) {
        boost::property_tree::read_info(conf_file_.string(), config_);
    } else {
        LOG_ERROR << "File " << log::Esc(conf_file_.string()) << " don't exist.";
        WAR_THROW_T(ExceptionNotFound, conf_file_.string());
    }

    threadpool_ = make_unique<war::Threadpool>(
        config_.get<unsigned>("service.io_threads", 2));

    protocol_ = ImProtocol::CreateProtocol([this]() -> war::Pipeline& {
        return threadpool_->GetAnyPipeline();
    }, config_);
    LoadBuddies();
    event_monitor_ = make_shared<Events>(*this);
    protocol_->SetMonitor(event_monitor_);
}


Api::Buddy::ptr_t ImManager::AddBuddy(const Buddy::Info& def)
{
    if (def.CanBeLogged()) {
        LOG_NOTICE_FN << "Adding buddy: " << def.id;
    } else {
        LOG_NOTICE_FN << "Adding anonymous buddy";
    }

    if (def.id.empty()) {
        WAR_THROW_T(war::ExceptionNoKeyValue, "id is required");
    }

    LOCK;
    auto buddy = make_shared<BuddyImpl>(def, shared_from_this());

    WarMapAddUnique(buddies_, def.id, buddy);
    // TODO: Trigger event that the buddy was added

    return buddy;
}

void ImManager::RemoveBuddy(const string& id)
{
    bool can_log = false;
    {
        LOCK;
        auto it = buddies_.find(id);
        if (it == buddies_.end()) {
            return;
        }

        can_log = it->second->CanBeLogged();

        buddies_.erase(it);
        // TODO Send event
    }

    if (can_log) {
        LOG_NOTICE_FN << "Removed buddy: " << id;
    } else {
        LOG_NOTICE_FN << "Removed anonymous buddy";
    }
}


void ImManager::Disconnect(bool local_only)
{
    assert(protocol_);

    // TODO: Trigger event that we are disconnecting
    protocol_->Shutdown();
}

Api::buddy_list_t ImManager::GetBuddies()
{
    buddy_list_t list;

    LOCK;
    for(auto it : buddies_) {
        list.push_back(it.second);
    }

    return list;
}

void ImManager::GoOnline(const Info& my_info)
{
    // TODO: Trigger event that we are going on-line

    const auto id =  config_.get<string>("service.dark_id");
    const auto hostname = config_.get("service.hostname", "127.0.0.1");
    const auto port = config_.get<unsigned short>("service.hostname", 11009);



    boost::asio::ip::tcp::endpoint endpoint{
        boost::asio::ip::address::from_string(hostname), port};

    LOG_NOTICE << "Starting listening for events on " << endpoint;

    protocol_->SetInfo(my_info);
    protocol_->Listen(endpoint);

    LOG_DEBUG_FN<< "I will now connect to all buddys with the auto_connect"
        " flag set for my id: " << log::Esc(id);

    LOCK;
    my_info_ = my_info;
    for(auto& it : buddies_) {
        auto& buddy = it.second;
        if (buddy->HasAutoConnect()) {
            protocol_->Connect(buddy);
        }
    }
}

void ImManager::Panic(std::string message, bool erase_data)
{
    assert(false && "Not implemented");
}


void ImManager::LoadBuddies()
{
    path_t path = config_.get("service.buddy_file", "buddies.data");

    if (boost::filesystem::is_regular(path)) {
        // open the archive
        std::ifstream ifs(path.string());
        boost::archive::text_iarchive ia(ifs);

        InfoData data;
        ia >> data;

        LOG_DEBUG_FN << "Loaded " << data.data.size() << " buddies from "
            << log::Esc(path.string());

        LOCK;
        std::weak_ptr<ImManager> weak_mgr = shared_from_this();
        assert(buddies_.empty());
        for(auto& d : data.data) {
            buddies_[d.id] = make_shared<BuddyImpl>(d, weak_mgr);
        }
    }
}


void ImManager::SaveBuddies()
{
    // TODO: Rename the old file to .bak, and restore it if we fail.
    InfoData data;
    data.data.reserve(buddies_.size());
    {
        LOCK;
        for(auto& b : buddies_) {
            auto info = b.second->GetInfo();
            if (info.CanBeSaved()) {
                data.data.push_back(move(info));
            }
        }
    }

    const auto path = config_.get("service.buddy_file", "buddies.data");

    LOG_DEBUG_FN << "Saving " << data.data.size() << " buddies to "
        << log::Esc(path);

    std::ofstream ofs(path);
    boost::archive::text_oarchive oa(ofs);
    oa << data;
}

void ImManager::Connect(Api::Buddy::ptr_t buddy)
{
    protocol_->Connect(move(buddy));
}

BuddyImpl::ptr_t ImManager::GetBuddy(const string& id)
{
    LOCK;
    auto it = buddies_.find(id);
    if (it != buddies_.end()) {
        return it->second;
    }

    return {};
}

bool ImManager::HaveBuddy(const string& id) const
{
    LOCK;
    return buddies_.find(id) != buddies_.end();
}


//////////////// Events //////////////

void ImManager::SetMonitor(shared_ptr<EventMonitor> monitor)
{
    LOCK;
    event_monitors_.push_back(monitor);
}


vector<shared_ptr<EventMonitor>> ImManager::GetMonitors()
{
    LOCK;
    return GetValidObjects<EventMonitor>(event_monitors_);
}


bool ImManager::Events::OnIncomingConnection(const EventMonitor::ConnectionInfo& info)
{
    for(auto& monitor : manager_.GetMonitors()) {
        if (!monitor->OnIncomingConnection(info)) {
            LOG_DEBUG_FN << "Connect from (potential) buddy " << info.buddy_id
                << " was denied.";
            return false;
        }
    }

    // TODO: Add blacklist

    LOG_DEBUG_FN << "Approving connect from (potential) buddy " << info.buddy_id;
    return true;
}


bool ImManager::Events::OnAddNewBuddy(const EventMonitor::BuddyInfo& info)
{
    for(auto& monitor : manager_.GetMonitors()) {
        if (!monitor->OnAddNewBuddy(info)) {
            LOG_DEBUG_FN << "Adding a buddy " << info.buddy_id
                << " was denied.";
            return false;
        }
    }

    if (manager_.HaveBuddy(info.buddy_id)) {
        return true;
    }

    if (manager_.GetConfigValue("settings.auto_accept_buddies", true)) {

        LOG_DEBUG_FN << "Adding incoming buddy " << info.buddy_id;

        Buddy::Info buddy;
        buddy.id = info.buddy_id;
        buddy.first_contact = buddy.last_seen = buddy.created_time = time(nullptr);
        buddy.profile_name = info.profile_name;
        buddy.profile_text = info.profile_text;
        manager_.AddBuddy(buddy);

        OnNewBuddyAdded(info);
        return true;
    }

    return false;
}


void ImManager::Events::OnNewBuddyAdded(const EventMonitor::BuddyInfo& info)
{
    for(auto& monitor : manager_.GetMonitors()) {
        monitor->OnNewBuddyAdded(info);
    }
}

void ImManager::Events::OnBuddyStateUpdate(const EventMonitor::BuddyInfo& info)
{
    auto buddy = manager_.GetBuddy(info.buddy_id);
    if (!buddy) {
        WAR_THROW_T(ExceptionDisconnectNow, "Nonexistant buddy");
    }

    if (buddy->GetStatus() != info.status) {
        buddy->OnStateChange(info.status);
    }

    for(auto& monitor : manager_.GetMonitors()) {
        monitor->OnBuddyStateUpdate(info);
    }
}

void ImManager::Events::OnIncomingMessage(const EventMonitor::Message& message)
{
    LOG_DEBUG << "Incoming message from " << log::Esc(message.buddy_id)
        << ": " << log::Esc(message.message);

    if (!message.buddy_id.empty()) {
        auto buddy = manager_.GetBuddy(message.buddy_id);
        if (buddy) {
            auto msg = make_shared<Api::Message>(
                Api::Message::Direction::INCOMING,
                Api::Message::Status::RECEIVED,
                message.message);
            buddy->OnMessageReceived(msg);
        }
    }

    for(auto& monitor : manager_.GetMonitors()) {
        monitor->OnIncomingMessage(message);
    }
}

void ImManager::Events::OnIncomingFile(const EventMonitor::FileInfo& file)
{
    for(auto& monitor : manager_.GetMonitors()) {
        monitor->OnIncomingFile(file);
    }
}

void ImManager::Events::OnOtherEvent(const EventMonitor::Event& event)
{
    if (!event.buddy_id.empty()) {
        auto buddy = manager_.GetBuddy(event.buddy_id);
        if (buddy) {
            buddy->OnOtherEvent(event);
        }
    }

    for(auto& monitor : manager_.GetMonitors()) {
        monitor->OnOtherEvent(event);
    }
}

void ImManager::Events::OnListening(const EventMonitor::ListeningInfo& endpoint)
{
    for(auto& monitor : manager_.GetMonitors()) {
        monitor->OnListening(endpoint);
    }
}

void ImManager::Events::OnShutdownComplete(const EventMonitor::ShutdownInfo& info)
{
    for(auto& monitor : manager_.GetMonitors()) {
        monitor->OnShutdownComplete(info);
    }
}

shared_ptr< ImManager > ImManager::CreateInstance(path_t conf_file)
{
    shared_ptr< ImManager > mgr{new ImManager(conf_file)};
    mgr->Init();
    return mgr;
}


} // impl

shared_ptr<Api> Api::CreateInstance(path_t conf_file)
{
    return impl::ImManager::CreateInstance(conf_file);
}

} // darkspeak
