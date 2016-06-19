
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

std::ostream& operator << (std::ostream& o, const darkspeak::EventMonitor::Event::Type& v) {
    static const array<string, 4> names = { "UNKNOWN", "MESSAGE_TRANSMITTED",
        "PROTOCOL_CONNECTING", "PROTOCOL_DISCONNECTING" };

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
    void serialize(Archive & ar, int version)
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
    void serialize(Archive & ar, int version)
    {
        ar & data;
    }
};

ImManager::ImManager(path_t conf_file)
{
    config_ = make_shared<Config>(conf_file);
}

ImManager::~ImManager()
{
    SaveBuddies();
    if (config_->IsDirty()) {
        config_->Save();
    }
}

void ImManager::Init()
{
    config_->Load();

    threadpool_ = make_unique<war::Threadpool>(
        config_->Get<unsigned>(Config::IO_THREADS, 2));

    protocol_ = ImProtocol::CreateProtocol([this]() -> war::Pipeline& {
        return threadpool_->GetAnyPipeline();
    }, *config_);
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

    std::shared_ptr<BuddyImpl> buddy;
    {
        LOCK;
        buddy = make_shared<BuddyImpl>(def, shared_from_this());
        WarMapAddUnique(buddies_, def.id, buddy);
    }
    EventMonitor::BuddyInfo bi;
    bi.buddy_id = def.id;
    bi.exists = EventMonitor::Existing::YES;
    bi.profile_name = def.profile_name;
    bi.profile_text = def.profile_text;

    for(auto& m : GetMonitors()) {
        m->OnNewBuddyAdded(bi);
    }

    return buddy;
}

void ImManager::RemoveBuddy(const string& id)
{
    Api::Buddy::ptr_t buddy;
    {
        LOCK;
        auto it = buddies_.find(id);
        if (it == buddies_.end()) {
            return;
        }

        buddy = it->second;
        buddies_.erase(it);
    }

    LOG_NOTICE_FN << "Removed buddy: " << *buddy;

    protocol_->Disconnect(*buddy);
    EventMonitor::DeletedBuddyInfo info;
    info.buddy_id = id;
    for(auto& monitor : GetMonitors()) {
        monitor->OnBuddyDeleted(info);
    }
}


void ImManager::Disconnect(bool local_only)
{
    assert(protocol_);

    EventMonitor::Event event{EventMonitor::Event::Type::PROTOCOL_DISCONNECTING};
    for(auto& monitor : GetMonitors()) {
        monitor->OnOtherEvent(event);
    }

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


void ImManager::GoOnline()
{
    // TODO: Trigger event that we are going on-line

    const auto id =  config_->Get<string>(Config::HANDLE);
    const auto hostname = config_->Get<string>(Config::SERVICE_HOSTNAME,
                                               Config::SERVICE_HOSTNAME_DEFAULT);
    const auto port = config_->Get<unsigned short>(Config::SERVICE_PORT,
                                                   Config::SERVICE_PORT_DEFAULT);

    boost::asio::ip::tcp::endpoint endpoint{
        boost::asio::ip::address::from_string(hostname), port};

    LOG_NOTICE << "Starting listening for events on " << endpoint;

    EventMonitor::Event event{EventMonitor::Event::Type::PROTOCOL_CONNECTING};
    for(auto& monitor : GetMonitors()) {
        monitor->OnOtherEvent(event);
    }

    protocol_->Listen(endpoint);

    LOG_DEBUG_FN<< "I will now connect to all buddys with the auto_connect"
        " flag set for my id: " << log::Esc(id);

    LOCK;
    for(auto& it : buddies_) {
        auto& buddy = it.second;
        if (buddy->HasAutoConnect()) {
            LOG_DEBUG << "Connecting to " << log::Esc(buddy->GetUiName())
                << " {id:" << buddy->GetId() << "}";
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
    path_t path = config_->Get(Config::SERVICE_BUDDY_FILE,
                               Config::SERVICE_BUDDY_FILE_DEFAULT);

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

    const auto path = config_->Get(Config::SERVICE_BUDDY_FILE,
                                   Config::SERVICE_BUDDY_FILE_DEFAULT);

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

    if (manager_.GetConfig()->Get("settings.auto_accept_buddies", true)) {

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

    if (info.status != Api::Status::OFF_LINE) {
        buddy->UpdateLastSeenTimestamp();
    }

    buddy->Update(info);

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
            buddy->UpdateLastSeenTimestamp();
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

// Api::Info impl::ImManager::GetInfo()
// {
//     Api::Info info;
//     info.id = config_->Get(Config::HANDLE);
//     info.profile_name = config_->Get(Config::PROFILE_NAME, "");
//     info.profile_text = config_->Get(Config::PROFILE_INFO, "");
//     switch(config_->Get<int>("profile.status", 0)) {
//         case 0: info.status = Api::Status::AVAILABLE; break;
//         case 1: info.status = Api::Status::AWAY; break;
//         case 2: info.status = Api::Status::LONG_TIME_AWAY; break;
//         default:
//             LOG_WARN_FN << "Invalid status. Resetting to available.";
//     }
//
//     return info;
// }


} // darkspeak
