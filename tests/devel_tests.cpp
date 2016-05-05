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

    LOG_INFO << "devel_tests starting up wityh cwd='"
        << boost::filesystem::current_path().string()
        << "'";

    auto manager = std::make_shared<impl::ImManager>("testing.conf");

    manager->GoOnline();

    return 0;
}

