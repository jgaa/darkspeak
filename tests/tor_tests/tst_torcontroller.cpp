#include "tst_torcontroller.h"

#include "ds/torcontroller.h"
#include "ds/torconfig.h"

void TestTorController::test_auth_cookie()
{
    ds::tor::TorConfig cfg;

    cfg.ctl_port = 9051;
    cfg.allowed_auth_methods.clear();
    cfg.allowed_auth_methods += "COOKIE";

    ds::tor::TorController ctl(cfg);
    QSignalSpy spy_connect(&ctl, SIGNAL(autenticated()));
    ctl.start();
    QCOMPARE(spy_connect.wait(1000), true);
}

void TestTorController::test_auth_safecookie()
{
    ds::tor::TorConfig cfg;

    cfg.ctl_port = 9051;
    cfg.allowed_auth_methods.clear();
    cfg.allowed_auth_methods += "SAFECOOKIE";

    ds::tor::TorController ctl(cfg);
    QSignalSpy spy_connect(&ctl, SIGNAL(autenticated()));
    ctl.start();
    QCOMPARE(spy_connect.wait(1000), true);
}
