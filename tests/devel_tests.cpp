/*
 * This file contains tests needed during development.
 *
 * Some tests will be refactored into regular unit tests,
 * other into functional tests, while some will be deleted.
 */

#include "war_basics.h"
#include "war_error_handling.h"
#include "log/WarLog.h"

#include "darkspeak/Api.h"
#include "darkspeak/ImManager.h"


using namespace std;
using namespace war;
using namespace darkspeak;

int main(int argc, char **argv) {

    log::LogEngine log_engine;
    log_engine.AddHandler(make_shared<war::log::LogToFile>(
        "devel_tests.log", true, "file", log::LL_TRACE4));

    log_engine.AddHandler(make_shared<war::log::LogToStream>(
        clog, "console", log::LL_DEBUG));

    LOG_INFO << "devel_tests starting up with cwd='"
        << boost::filesystem::current_path().string()
        << "'";

    auto manager = std::make_shared<impl::ImManager>("testing.conf");

    Api::Info me;
    auto config = manager->GetConfig();
    me.id = config.get<string>("service.dark_id");
    me.profile_name = config.get("profile.name", "");
    me.profile_text = config.get("profile.text", "");
    me.status = Api::Status::AWAY;
    manager->GoOnline(me);

    auto test_buddy = config.get("connect_test.buddy.id", "");
    if (!test_buddy.empty()) {
        Api::Buddy::Info b;
        b.id = test_buddy;
        b.auto_connect = true;
        auto buddy = manager->AddBuddy(b);
        buddy->Connect();
        buddy->SendMessage("=============.");
        buddy->SendMessage("I am testing connect.");
        buddy->SendMessage("I am testing queuing.");
        buddy->SendMessage("I am testing more queuing.");
        buddy->SendMessage("I am testing spamming.");
        buddy->SendMessage("This is the last message I will\nsend in this connection.");
    }

    string ttt;
    cin >> ttt; // wait

    return 0;
}

