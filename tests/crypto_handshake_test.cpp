#include <QtTest>

#include <sodium.h>

#include "ds/certimpl.h"
#include "ds/crypto.h"
#include "ds/dscert.h"

using ds::crypto::CertImpl;
using ds::crypto::DsCert;

namespace {
QByteArray bytes(const unsigned char *data, size_t size)
{
    return QByteArray(reinterpret_cast<const char *>(data), static_cast<int>(size));
}

QByteArray sign(DsCert::ptr_t cert, std::initializer_list<QByteArray> parts)
{
    QByteArray signature;
    signature.resize(crypto_sign_BYTES);
    crypto_sign_state state{};
    crypto_sign_init(&state);
    for (const auto &part : parts) {
        crypto_sign_update(&state,
                           reinterpret_cast<const unsigned char *>(part.constData()),
                           static_cast<size_t>(part.size()));
    }
    unsigned long long length = 0;
    if (crypto_sign_final_create(&state,
                                 reinterpret_cast<unsigned char *>(signature.data()),
                                 &length, cert->getSigningKey().cdata()) != 0) {
        throw std::runtime_error("signature creation failed");
    }
    return signature;
}
}

class CryptoHandshakeTest final : public QObject
{
    Q_OBJECT

private slots:
    void keyGenerationAndPublicRoundTrip()
    {
        ds::crypto::Crypto crypto;
        const auto identity = DsCert::create();
        const auto restored = DsCert::create(identity->getCert());
        const auto publicOnly = DsCert::createFromPubkey(identity->getSigningPubKey().toByteArray());

        QCOMPARE(restored->getSigningPubKey(), identity->getSigningPubKey());
        QCOMPARE(restored->getEncryptionPubKey(), identity->getEncryptionPubKey());
        QCOMPARE(restored->getHash(), identity->getHash());
        QCOMPARE(publicOnly->getSigningPubKey(), identity->getSigningPubKey());
        QCOMPARE(publicOnly->getEncryptionPubKey(), identity->getEncryptionPubKey());
        QVERIFY(publicOnly->getSigningKey().empty());
        QVERIFY(publicOnly->getEncryptionKey().empty());
    }

    void sealedPayloadAndSignature()
    {
        ds::crypto::Crypto crypto;
        const auto sender = DsCert::create();
        const auto recipient = DsCert::create();
        const QByteArray version(1, '\1');
        const QByteArray sessionKey = bytes(reinterpret_cast<const unsigned char *>("01234567890123456789012345678901"), 32);
        const QByteArray header = bytes(reinterpret_cast<const unsigned char *>("0123456789012345678901234567890123456789012345678901234567890123"), 24);
        const QByteArray payload = version + sessionKey + header + sender->getSigningPubKey().toByteArray();
        const auto signature = sign(sender, {payload});

        QByteArray sealed(payload.size() + crypto_box_SEALBYTES, Qt::Uninitialized);
        QVERIFY(crypto_box_seal(reinterpret_cast<unsigned char *>(sealed.data()),
                                reinterpret_cast<const unsigned char *>(payload.constData()),
                                static_cast<size_t>(payload.size()),
                                recipient->getEncryptionPubKey().cdata()) == 0);
        QByteArray opened(payload.size(), Qt::Uninitialized);
        QVERIFY(recipient->decrypt(opened, sealed));
        QCOMPARE(opened, payload);

        const auto senderPublic = DsCert::createFromPubkey(sender->getSigningPubKey().toByteArray());
        QVERIFY(senderPublic->verify(signature, {payload}));
        auto tampered = payload;
        tampered[0] = '\2';
        QVERIFY(!senderPublic->verify(signature, {tampered}));
    }

    void malformedCertificateAndCryptoInputsFailClosed()
    {
        QVERIFY_EXCEPTION_THROWN(DsCert::create(QByteArray(7, '\0')), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(DsCert::createFromPubkey(QByteArray(7, '\0')), std::runtime_error);

        ds::crypto::Crypto crypto;
        const auto identity = DsCert::create();
        const auto publicOnly = DsCert::createFromPubkey(identity->getSigningPubKey().toByteArray());
        QByteArray signature(crypto_sign_BYTES, Qt::Uninitialized);
        QVERIFY_EXCEPTION_THROWN(publicOnly->sign(signature, {QByteArray("payload")}), std::runtime_error);
        QVERIFY(!publicOnly->verify(QByteArray(crypto_sign_BYTES - 1, '\0'),
                                    {QByteArray("payload")}));

        QByteArray opened(1, Qt::Uninitialized);
        QVERIFY(!identity->decrypt(opened, QByteArray(crypto_box_SEALBYTES - 1, '\0')));
    }

    void secretStreamCaptureRoundTrip()
    {
        ds::crypto::Crypto crypto;
        QByteArray key(crypto_secretstream_xchacha20poly1305_KEYBYTES, Qt::Uninitialized);
        crypto_secretstream_xchacha20poly1305_keygen(reinterpret_cast<unsigned char *>(key.data()));
        QByteArray header(crypto_secretstream_xchacha20poly1305_HEADERBYTES, Qt::Uninitialized);
        crypto_secretstream_xchacha20poly1305_state push{};
        QVERIFY(crypto_secretstream_xchacha20poly1305_init_push(
                    &push, reinterpret_cast<unsigned char *>(header.data()),
                    reinterpret_cast<const unsigned char *>(key.constData())) == 0);

        const QByteArray frame("v1|channel=0|request=1|payload=handshake-capture");
        QByteArray ciphertext(frame.size() + crypto_secretstream_xchacha20poly1305_ABYTES,
                               Qt::Uninitialized);
        QVERIFY(crypto_secretstream_xchacha20poly1305_push(
                    &push, reinterpret_cast<unsigned char *>(ciphertext.data()), nullptr,
                    reinterpret_cast<const unsigned char *>(frame.constData()),
                    static_cast<unsigned long long>(frame.size()), nullptr, 0,
                    crypto_secretstream_xchacha20poly1305_TAG_MESSAGE) == 0);

        crypto_secretstream_xchacha20poly1305_state pull{};
        QVERIFY(crypto_secretstream_xchacha20poly1305_init_pull(
                    &pull, reinterpret_cast<const unsigned char *>(header.constData()),
                    reinterpret_cast<const unsigned char *>(key.constData())) == 0);
        QByteArray opened(ciphertext.size(), Qt::Uninitialized);
        unsigned char tag = 0;
        QVERIFY(crypto_secretstream_xchacha20poly1305_pull(
                    &pull, reinterpret_cast<unsigned char *>(opened.data()), nullptr, &tag,
                    reinterpret_cast<const unsigned char *>(ciphertext.constData()),
                    static_cast<unsigned long long>(ciphertext.size()), nullptr, 0) == 0);
        opened.resize(frame.size());
        QCOMPARE(opened, frame);
        QCOMPARE(tag, static_cast<unsigned char>(crypto_secretstream_xchacha20poly1305_TAG_MESSAGE));
    }
};

QTEST_MAIN(CryptoHandshakeTest)
#include "crypto_handshake_test.moc"
