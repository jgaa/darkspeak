


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
    info.avatar = Config::DecodeRgba(
        Get<string>(Config::PROFILE_AVATAR_RGBA, ""));

    return info;
}

string Config::EncodeRgba(const vector< char >& binary)
{
    static const array<const char *, 256> hextable  {
        "00", "01", "02", "03", "04", "05", "06", "07", "08", "09", "0a", "0b", "0c", "0d", "0e", "0f", "10", "11",
        "12", "13", "14", "15", "16", "17", "18", "19", "1a", "1b", "1c", "1d", "1e", "1f", "20", "21", "22", "23",
        "24", "25", "26", "27", "28", "29", "2a", "2b", "2c", "2d", "2e", "2f", "30", "31", "32", "33", "34", "35",
        "36", "37", "38", "39", "3a", "3b", "3c", "3d", "3e", "3f", "40", "41", "42", "43", "44", "45", "46", "47",
        "48", "49", "4a", "4b", "4c", "4d", "4e", "4f", "50", "51", "52", "53", "54", "55", "56", "57", "58", "59",
        "5a", "5b", "5c", "5d", "5e", "5f", "60", "61", "62", "63", "64", "65", "66", "67", "68", "69", "6a", "6b",
        "6c", "6d", "6e", "6f", "70", "71", "72", "73", "74", "75", "76", "77", "78", "79", "7a", "7b", "7c", "7d",
        "7e", "7f", "80", "81", "82", "83", "84", "85", "86", "87", "88", "89", "8a", "8b", "8c", "8d", "8e", "8f",
        "90", "91", "92", "93", "94", "95", "96", "97", "98", "99", "9a", "9b", "9c", "9d", "9e", "9f", "a0", "a1",
        "a2", "a3", "a4", "a5", "a6", "a7", "a8", "a9", "aa", "ab", "ac", "ad", "ae", "af", "b0", "b1", "b2", "b3",
        "b4", "b5", "b6", "b7", "b8", "b9", "ba", "bb", "bc", "bd", "be", "bf", "c0", "c1", "c2", "c3", "c4", "c5",
        "c6", "c7", "c8", "c9", "ca", "cb", "cc", "cd", "ce", "cf", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
        "d8", "d9", "da", "db", "dc", "dd", "de", "df", "e0", "e1", "e2", "e3", "e4", "e5", "e6", "e7", "e8", "e9",
        "ea", "eb", "ec", "ed", "ee", "ef", "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "fa", "fb",
        "fc", "fd", "fe", "ff"
    };

    std::ostringstream hex;
    for(const auto ch : binary) {
        hex << hextable[static_cast<uint8_t>(ch)];
    }

    auto rval = move(hex.str());

    LOG_DEBUG_FN << "After encode binary is " << binary.size()
        << " and rval is " << rval.size();

    return rval;
}


vector< char > Config::DecodeRgba(const string& hex)
{
    vector< char > rgba;

    static const array<char, 16> alphabet{'0','1','2','3','4','5','6',
        '7','8','9','a','b','c','d','e','f'};
    const auto len = hex.size();
    if (len % 1) {
        WAR_THROW_T(ExceptionOutOfRange, "Length must be aligned with 2");
    }

    rgba.reserve(len / 2);
    auto ch = hex.cbegin();
    for (size_t i = 0; i < len; i += 2)
    {
        const char* p = std::lower_bound(alphabet.cbegin(),
                                         alphabet.cend(),
                                         *ch);
        if (*p != *ch) {
            WAR_THROW_T(ExceptionOutOfRange, "Not a hex digit");
        }

        ++ch;

        const char* q =  std::lower_bound(alphabet.cbegin(),
                                         alphabet.cend(),
                                         *ch);
        if (*q != *ch) {
            WAR_THROW_T(ExceptionOutOfRange, "Not a hex digit");
        }

        ++ch;

        rgba.push_back(((p - alphabet.data()) << 4) | (q - alphabet.data()));
    }

    LOG_DEBUG_FN << "After decode hex is " << hex.size()
        << " and rgba is " << rgba.size();

    return rgba;
}


} // namespace
