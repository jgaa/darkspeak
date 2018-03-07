#include "tst_certs.h"

#include "ds/dscert.h"

void TestCerts::test_create_cert()
{
    auto first = ds::crypto::DsCert::create(ds::crypto::DsCert::Type::RSA_512);

    QVERIFY(first != nullptr);

    auto cert = first->getCert();
    QVERIFY(!cert.isEmpty());

    auto pubkey = first->getPubKey();
    QVERIFY(!pubkey.isEmpty());


    // Create a new cert from the cert received above
    try {
        auto second = ds::crypto::DsCert::create(cert);

        QCOMPARE(cert, second->getCert());
        QCOMPARE(pubkey, second->getPubKey());
    } catch(const std::exception& ex) {
        auto msg = std::string{"Caught exception: "} + ex.what();
        QFAIL(msg.c_str());
    }
}
