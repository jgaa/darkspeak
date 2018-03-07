#include "tst_torcontroller.h"

#include "ds/torcontroller.h"
#include "ds/torconfig.h"

void TestTorController::test_auth_cookie()
{
    ds::tor::TorConfig cfg;

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

    cfg.allowed_auth_methods.clear();
    cfg.allowed_auth_methods += "SAFECOOKIE";

    ds::tor::TorController ctl(cfg);
    QSignalSpy spy_connect(&ctl, SIGNAL(autenticated()));
    ctl.start();
    QCOMPARE(spy_connect.wait(1000), true);
}

// To run this test, create a hashed password by running
// tor --hash-password password and put the hash in torrc.
// Then start the test with the environment variable DS_QT_TEST_TOR_PWD set
// to that password.
void TestTorController::test_auth_hashedpassword()
{
    ds::tor::TorConfig cfg;
    cfg.allowed_auth_methods.clear();
    cfg.allowed_auth_methods += "HASHEDPASSWORD";
    cfg.ctl_passwd = qgetenv("DS_QT_TEST_TOR_PWD");

    if (cfg.ctl_passwd.isEmpty()) {
        qWarning() << "No DS_QT_TEST_TOR_PWD enviroment-variable. Password-test is disabled";
        return;
    }

    ds::tor::TorController ctl(cfg);
    QSignalSpy spy_connect(&ctl, SIGNAL(autenticated()));
    ctl.start();
    QCOMPARE(spy_connect.wait(1000), true);
}

void TestTorController::test_ready()
{
    ds::tor::TorConfig cfg;
    ds::tor::TorController ctl(cfg);
    QSignalSpy spy_connect(&ctl, SIGNAL(ready()));
    ctl.start();
    QCOMPARE(spy_connect.wait(2000), true);
}

void TestTorController::test_create_service()
{
    ds::tor::TorConfig cfg;
    ds::tor::TorController ctl(cfg);
    QSignalSpy spy_connect(&ctl, SIGNAL(ready()));
    ctl.start();
    QCOMPARE(spy_connect.wait(2000), true);

    QSignalSpy spy_started(&ctl, SIGNAL(serviceStarted(const QByteArray&)));
    ctl.createService("test");
    QCOMPARE(spy_started.wait(2000), true);
}

void TestTorController::test_start_service()
{
    ds::tor::TorConfig cfg;
    cfg.app_port = 12345; // TODO: Select a port no app listen to
    ds::tor::TorController ctl(cfg);
    QSignalSpy spy_connect(&ctl, SIGNAL(ready()));
    ctl.start();
    QCOMPARE(spy_connect.wait(2000), true);

    QSignalSpy spy_created(&ctl, SIGNAL(serviceCreated(const ServiceProperties&)));
    ctl.createService("test");
    QCOMPARE(spy_created.wait(2000), true);
    auto signal = spy_created.takeFirst();
    auto service = signal.at(0).value<::ds::tor::ServiceProperties>();

    QSignalSpy spy_stopped(&ctl, SIGNAL(serviceStopped(const QByteArray&)));
    ctl.stopService(service.id);
    QCOMPARE(spy_stopped.wait(2000), true);

    QSignalSpy spy_started(&ctl, SIGNAL(serviceStarted(const QByteArray&)));
    ctl.startService(service);
    QCOMPARE(spy_started.wait(2000), true);
}
