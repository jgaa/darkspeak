#include "tst_certs.h"

#include "ds/dscert.h"

void TestCerts::test_create_cert()
{
    auto first = ds::crypto::DsCert::create();

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

        qDebug() << "Created cert: " << first->getCert().toBase64();
        qDebug() << "         key: " << first->getKey().toBase64();
        qDebug() << "      pubkey: " << first->getPubKey().toBase64();
    } catch(const std::exception& ex) {
        auto msg = std::string{"Caught exception: "} + ex.what();
        QFAIL(msg.c_str());
    }
}
