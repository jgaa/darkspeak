


#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/info_parser.hpp>

#include "log/WarLog.h"

#include "darkspeak/darkspeak.h"
#include "darkspeak/Api.h"
#include "darkspeak/Config.h"

using namespace std;
using namespace war;

namespace darkspeak {

Config::Config(const path_t path)
: conf_file_{path}
{
}

void Config::Load()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (boost::filesystem::exists(conf_file_)) {
        boost::property_tree::read_info(conf_file_.string(), config_);
    } else {
        LOG_ERROR << "File " << log::Esc(conf_file_.string()) << " don't exist.";
        WAR_THROW_T(ExceptionNotFound, conf_file_.string());
    }

    dirty_ = false;
}

void Config::Save()
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto tmp_path = conf_file_;
    tmp_path += "~";
    auto bak_file = conf_file_;
    bak_file += ".bak";

    boost::property_tree::write_info(tmp_path.string(), config_);

    if (boost::filesystem::is_regular_file(conf_file_)) {
        boost::filesystem::rename(conf_file_, bak_file);
    }
    boost::filesystem::rename(tmp_path, conf_file_);

    dirty_ = false;
}

Api::Info Config::GetInfo()
{
    Api::Info info;

    info.id = Get(Config::HANDLE);
    info.profile_name = Get(Config::PROFILE_NAME, "");
    info.profile_text = Get(Config::PROFILE_INFO, "");
    switch(Get<int>("profile.status", 0)) {
        case 0: info.status = Api::Status::AVAILABLE; break;
        case 1: info.status = Api::Status::AWAY; break;
        case 2: info.status = Api::Status::LONG_TIME_AWAY; break;
        default:
            LOG_WARN_FN << "Invalid status. Resetting to available.";
    }

    return info;
}


} // namespace
