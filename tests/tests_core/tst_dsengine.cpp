
#include <memory>
#include <QSettings>

#include "tst_dsengine.h"
#include "ds/dsengine.h"


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
    QByteArray cert{"-----BEGIN RSA PRIVATE KEY-----\n"
                    "MIICXQIBAAKBgQDQi1vRvVZbzXhMGyeiz6GaKFGXVBIxhxkMXRRMcbHgn8Mx1rxt\n"
                    "1VfPyJvuu22lpHV9N+hYQmguY8T9eyyXJ2z2K2cKGZoR6G4giKjLa2lvG5BSChHU\n"
                    "rEG2mz5MpvOE203aTBW0DmADUFDVAvDivASyx2KzxlaK4sUBjBD4RgwY6wIEAQUY\n"
                    "tQKBgBnkbvU7NsRb44LI91OkyU49ui2U0qbccCgx4/6aUr4x3BUS+RT7bN9Wu+5m\n"
                    "cLr1ExHNnxdKnJJ8k1MfmehaSniq5/mJwYxfCD6bsaiqYmn10lf47RzEmC4T+a/0\n"
                    "zMUU+32d8FbczpSelSJP+X1syVWqIMfTmCzDBM88ChFwK6z9AkEA8Oz39TXz9SpA\n"
                    "aPmnMHGtfZpj7TKYgLmT2BnFIeJUZI0DmjsqdqM78izHZ59Ci2M30Lg6DMUnzkBf\n"
                    "46LinTzsbwJBAN2XuN+lNiVFifdsSrPnBocQYQgpt9N++l7IQICZ9RiG4+wTaoDR\n"
                    "R8nM6iTx+5MyFzS4brN+4YYXR1ElPDZjEUUCQQCFaWCpm5Ok0y0Ffl9hxXQPhyqe\n"
                    "jIaEWhQZOKxCimecp9UOiJQCx5uYFR06kISChpFeMCGkjW6JPyPTZKZxGYxXAkAa\n"
                    "HJ1k1owVn5Jd5FWnrk2jsonWwo4Myc5asX7GpSsAadba9m9XBsHvHLvQb6SqAdsd\n"
                    "Y0B84m8pt0IGJLBs65MNAkBpd8mZQiLc1VHDY/2NPHaC2Fy4sdI/y38DFGKnwD/l\n"
                    "LYnD7uFnhVVU+avNatvOpEkTInRVy/DWHxYh3gWv44hl\n"
                    "-----END RSA PRIVATE KEY-----"};
    QByteArray addr{"onion:dstest3wkx5oyral:12345"};

    auto handle = ds::core::DsEngine::getIdentityHandle(cert, addr);

    QVERIFY(!handle.isEmpty());

    qDebug() << "handle is: " << handle;
}

void TestDsEngine::test_create_contact()
{
    QByteArray cert{"-----BEGIN RSA PRIVATE KEY-----\n"
                    "MIICXQIBAAKBgQDQi1vRvVZbzXhMGyeiz6GaKFGXVBIxhxkMXRRMcbHgn8Mx1rxt\n"
                    "1VfPyJvuu22lpHV9N+hYQmguY8T9eyyXJ2z2K2cKGZoR6G4giKjLa2lvG5BSChHU\n"
                    "rEG2mz5MpvOE203aTBW0DmADUFDVAvDivASyx2KzxlaK4sUBjBD4RgwY6wIEAQUY\n"
                    "tQKBgBnkbvU7NsRb44LI91OkyU49ui2U0qbccCgx4/6aUr4x3BUS+RT7bN9Wu+5m\n"
                    "cLr1ExHNnxdKnJJ8k1MfmehaSniq5/mJwYxfCD6bsaiqYmn10lf47RzEmC4T+a/0\n"
                    "zMUU+32d8FbczpSelSJP+X1syVWqIMfTmCzDBM88ChFwK6z9AkEA8Oz39TXz9SpA\n"
                    "aPmnMHGtfZpj7TKYgLmT2BnFIeJUZI0DmjsqdqM78izHZ59Ci2M30Lg6DMUnzkBf\n"
                    "46LinTzsbwJBAN2XuN+lNiVFifdsSrPnBocQYQgpt9N++l7IQICZ9RiG4+wTaoDR\n"
                    "R8nM6iTx+5MyFzS4brN+4YYXR1ElPDZjEUUCQQCFaWCpm5Ok0y0Ffl9hxXQPhyqe\n"
                    "jIaEWhQZOKxCimecp9UOiJQCx5uYFR06kISChpFeMCGkjW6JPyPTZKZxGYxXAkAa\n"
                    "HJ1k1owVn5Jd5FWnrk2jsonWwo4Myc5asX7GpSsAadba9m9XBsHvHLvQb6SqAdsd\n"
                    "Y0B84m8pt0IGJLBs65MNAkBpd8mZQiLc1VHDY/2NPHaC2Fy4sdI/y38DFGKnwD/l\n"
                    "LYnD7uFnhVVU+avNatvOpEkTInRVy/DWHxYh3gWv44hl\n"
                    "-----END RSA PRIVATE KEY-----"};
    QByteArray addr{"onion:testpzpcswyktnpd:12345"};

    auto settings = std::make_unique<QSettings>();
    settings->clear();
    settings->setValue("dbpath", ":memory:");
    ds::core::DsEngine engine(std::move(settings));
    Verifier verifier;
    connect(&engine, &ds::core::DsEngine::contactCreated, &verifier, &Verifier::contactCreated);

    ds::core::ContactReq cr;
    cr.name = "test";
    cr.contactHandle = engine.getIdentityHandle(cert, addr);

    engine.createContact(cr);
    QCOMPARE(verifier.verified, true);
}
