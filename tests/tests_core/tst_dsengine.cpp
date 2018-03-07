
#include <memory>
#include <QSettings>

#include "tst_dsengine.h"
#include "ds/dsengine.h"

TestDsEngine::TestDsEngine()
{

}

void TestDsEngine::test_create_identity()
{
    auto settings = std::make_unique<QSettings>();
    settings->clear();
    settings->setValue("dbpath", ":memory:");
    ds::core::DsEngine engine(std::move(settings));

    // Start the engine, connect to Tor, get ready
    {
        QSignalSpy spy_ready(&engine, SIGNAL(ready()));
        engine.start();
        QCOMPARE(spy_ready.wait(3000), true);
    }

    // Create an identity.
    // Will be ready when the cert and the hidden service is ready
    {
        QSignalSpy spy_created(&engine, SIGNAL(identityCreated(const Identity&)));
        ds::core::IdentityReq req;
        req.name = "testid";
        engine.createIdentity(req);
        QCOMPARE(spy_created.wait(5000), true);
        engine.close();
    }

}

