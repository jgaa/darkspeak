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

void TestCerts::test_signing()
{
    auto alice = ds::crypto::DsCert::create();
    auto bob = ds::crypto::DsCert::create();
    const auto bobpk_bytes = bob->getSigningPubKey().toByteArray();
    const auto bob_pk = ds::crypto::DsCert::createFromPubkey(bobpk_bytes);

    QCOMPARE(bob->getSigningPubKey(), bob_pk->getSigningPubKey());
    QCOMPARE(bob->getEncryptionPubKey(), bob_pk->getEncryptionPubKey());

    // Bob signs a message
    const QByteArray message = "This is a test";
    std::array<uint8_t, crypto_sign_BYTES> signature = {};

    bob->sign(signature, {message});
    // Verify with his original "cert" instance.
    QVERIFY(bob->verify(signature, {message}));

    // Verify with his public key "cert" instance, derived from his public key.
    QVERIFY(bob_pk->verify(signature, {message}));

    // Alices cert should not be able to verify this signature
    QVERIFY(!alice->verify(signature, {message}));

    const QByteArray another = "This is another test";
    bob->sign(signature, {message, another});
    // Verify with his original "cert" instance.
    QVERIFY(bob->verify(signature, {message, another}));

    // Verify with his public key "cert" instance, derived from his public key.
    QVERIFY(bob_pk->verify(signature, {message, another}));

    // Alices cert should not be able to verify this signature
    QVERIFY(!alice->verify(signature, {message, another}));

    // Not the same message tat was signed
    QVERIFY(!bob->verify(signature, {message}));
    QVERIFY(!bob->verify(signature, {another}));
    QVERIFY(!bob->verify(signature, {another, message}));
    QVERIFY(!bob->verify(signature, {message, another, message}));

}

void TestCerts::test_ppk_encryption()
{
    auto bob = ds::crypto::DsCert::create();
    const auto bobpk_bytes = bob->getSigningPubKey().toByteArray();
    const auto bob_pk = ds::crypto::DsCert::createFromPubkey(bobpk_bytes);

    QCOMPARE(bob->getEncryptionPubKey(), bob_pk->getEncryptionPubKey());

    // Derive the crypto pubkey from the actual key and verify that the pubkey in the cert matches.
    std::array<uint8_t, crypto_box_PUBLICKEYBYTES> pkey = {};
    crypto_scalarmult_base(pkey.data(), bob->getEncryptionKey().cdata());
    QVERIFY(pkey.size() == bob->getEncryptionPubKey().size());
    QVERIFY(memcmp(pkey.data(), bob->getEncryptionPubKey().data(), pkey.size()) == 0);

    const QByteArray message = "This is a test";
    QByteArray ciphertext, decrypted;
    ciphertext.resize(message.size() + static_cast<int>(crypto_box_SEALBYTES));
    decrypted.resize(message.size());

    bob_pk->encrypt(ciphertext, message);
    QVERIFY(bob->decrypt(decrypted, ciphertext));
    QCOMPARE(message, decrypted);

    // Other certs must not be able to decrypt
    auto voldemort = ds::crypto::DsCert::create();
    QVERIFY(!voldemort->decrypt(decrypted, ciphertext));
}
