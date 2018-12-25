
#include <memory>
#include <QSettings>

#include "tst_dsengine.h"
#include "ds/dsengine.h"

#include "logfault/logfault.h"

void Verifier::contactCreated(const ds::core::Contact&) {
    verified = true;
}

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

void TestDsEngine::test_create_identity_when_still_offline()
{
    auto settings = std::make_unique<QSettings>();
    settings->clear();
    settings->setValue("dbpath", ":memory:");
    ds::core::DsEngine engine(std::move(settings));

    // Create an identity.
    // Will be ready when the cert and the hidden service is ready
    {
        QSignalSpy spy_created(&engine, SIGNAL(identityCreated(const Identity&)));
        ds::core::IdentityReq req;
        req.name = "testid";
        engine.createIdentity(req);
        engine.start();
        QCOMPARE(spy_created.wait(5000), true);
        engine.close();
    }

}

void TestDsEngine::test_get_identity_handle()
{
    const auto cert = QByteArray::fromBase64("TX0l2CDyVR/E9peviEe5gIemqZZ3ecH3LO5wtlQlPC/VlT9htg+yEeuqr7ylw9cBrIRBpONP0A1YYGUbhxqMcg==");
    QByteArray addr{"onion:dstest3wkx5oyral:12345"};

    auto handle = ds::core::DsEngine::getIdentityHandle(cert, addr);

    QVERIFY(!handle.isEmpty());

    LFLOG_DEBUG << "handle is: " << handle;
}

void TestDsEngine::test_create_contact()
{
    const auto pubkey = QByteArray::fromBase64("jvYAcNYJ0RIVu2axMQ5CIm7m9Cf08+dVz8Q/bgb4lzA=");
    QByteArray addr{"onion:testpzpcswyktnpd:12345"};

    auto settings = std::make_unique<QSettings>();
    settings->clear();
    settings->setValue("dbpath", ":memory:");
    ds::core::DsEngine engine(std::move(settings));
    Verifier verifier;
    connect(&engine, &ds::core::DsEngine::contactCreated, &verifier, &Verifier::contactCreated);

    ds::core::ContactReq cr;
    cr.name = "test";
    cr.contactHandle = engine.getIdentityHandle(pubkey, addr);

    engine.createContact(cr);
    QCOMPARE(verifier.verified, true);
}
