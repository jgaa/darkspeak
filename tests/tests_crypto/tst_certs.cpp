#include "tst_certs.h"
#include "logfault/logfault.h"
#include "ds/dscert.h"
#include "ds/base58.h"

void TestCerts::test_create_cert()
{
    auto first = ds::crypto::DsCert::create();

    QVERIFY(first != nullptr);

    auto cert = first->getCert();
    QVERIFY(!cert.isEmpty());

    auto pubkey = first->getSigningPubKey();
    QVERIFY(!pubkey.isEmpty());


    // Create a new cert from the cert received above
    try {
        auto second = ds::crypto::DsCert::create(cert);

        QCOMPARE(cert, second->getCert());
        QCOMPARE(pubkey, second->getSigningPubKey());
        QCOMPARE("ds", ds::crypto::b58check_enc(first->getSigningPubKey(), {249, 50}).substr(0,2));

        LFLOG_DEBUG << "  Created cert: " << first->getCert().toByteArray().toBase64();
        LFLOG_DEBUG << "           key: " << first->getSigningKey().toByteArray().toBase64();
        LFLOG_DEBUG << "        pubkey: " << first->getSigningPubKey().toByteArray().toBase64();

//        for(unsigned char i = 1; i < 255; ++i) {
//            for(unsigned char ii = 0; ii < 255; ++ii) {
//                auto handle = ds::crypto::b58check_enc(first->getPubKey(), {i, ii});
//                if (handle.at(0) == 'd' && handle.at(1) == 's') {
//                    LFLOG_DEBUG << "     handle: " << handle << ' ' << (int)i << ' ' << (int)ii;
//                }
//            }
//        }
        auto handle = ds::crypto::b58check_enc(first->getSigningPubKey(), {249, 50});
        LFLOG_DEBUG << "        handle: " << handle;
        LFLOG_DEBUG << "decoded handle: " << ds::crypto::b58tobin_check<QByteArray>(handle,
                                                                                    first->getSigningPubKey().size(), {249, 50}).toBase64();
    } catch(const std::exception& ex) {
        auto msg = std::string{"Caught exception: "} + ex.what();
        QFAIL(msg.c_str());
    }
}
