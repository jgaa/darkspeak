
#pragma once

#include <string>
#include <memory>
#include <mutex>

#include <boost/property_tree/ptree.hpp>

namespace darkspeak {

class Config {
public:
    Config(const path_t path);

    void Load();
    void Save();

    template <typename T>
    T Get(const std::string& key, const T& def) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_.get(key, def);
    }

    template <typename T = std::string>
    T Get(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_.get<T>(key);
    }

    template <typename T = std::string>
    void Set(const std::string& key, const T& val) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_.put(key, val);
        dirty_ = true;
    }

    std::string Get(const std::string& key, const char *def) {
        return Get<std::string>(key, def);
    }

    bool Have(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_.find(key) != config_.not_found();
    }

    bool IsDirty() {
        std::lock_guard<std::mutex> lock(mutex_);
        return dirty_;
    }

    static std::string EncodeRgba(const std::vector<char>& binary);
    static std::vector<char> DecodeRgba(const std::string& hex);
    static std::vector<char> GetRgb(const std::vector<char>& binary);
    static std::vector<char> GetAlpha(const std::vector<char>& binary);

    Api::Info GetInfo();

    constexpr static const char *HANDLE = "service.dark_id";
    constexpr static const char *PROFILE_NAME = "profile.name";
    constexpr static const char *PROFILE_INFO = "profile.info";
    constexpr static const char *PROFILE_STATUS = "profile.status";
    constexpr static const int   PROFILE_STATUS_DEFAULT = 0;
    constexpr static const char *PROFILE_AVATAR_RGBA = "profile.avatar_argb";
    constexpr static const char *IO_THREADS = "service.io_threads";
    constexpr static const char *SERVICE_HOSTNAME = "service.hostname";
    constexpr static const char *SERVICE_HOSTNAME_DEFAULT = "127.0.0.1";
    constexpr static const char *SERVICE_PORT = "service.port";
    constexpr static const int   SERVICE_PORT_DEFAULT = 11009;
    constexpr static const char *SERVICE_BUDDY_FILE = "service.buddy_file";
    constexpr static const char *SERVICE_BUDDY_FILE_DEFAULT = "buddies.data";

    constexpr static const char *TOR_HOSTNAME = "tor.hostname";
    constexpr static const char *TOR_HOSTNAME_DEFAULT = "127.0.0.1";
    constexpr static const char *TOR_PORT = "tor.port";
    constexpr static const int   TOR_PORT_DEFAULT = 9050;

    constexpr static const char *DOWNLOAD_FOLDER = "folder.download";
    constexpr static const char *DOWNLOAD_FOLDER_DEFAULT = "./incoming/{id}";

    constexpr static const char *MAX_FILE_TRANSFERS_PER_CONTACT = "limits.max_transfers_per_contact";
    constexpr static const int MAX_FILE_TRANSFERS_PER_CONTACT_DEFAULT = 5;
    constexpr static const char *MAX_OUT_BUFFERS_PER_FILE_TRANSFER
        = "limits.max_out_buffers_per_file_transfer";
    constexpr static const int MAX_OUT_BUFFERS_PER_FILE_TRANSFER_DEFAULT = 12;

private:
    bool dirty_ = false;
    path_t conf_file_;
    mutable std::mutex mutex_;
    boost::property_tree::ptree config_;
};

} // namespace
