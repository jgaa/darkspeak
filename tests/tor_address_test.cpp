#include <QtTest>

#include <QCryptographicHash>

#include "ds/base32.h"
#include "ds/toraddress.h"

using ds::tor::TorAddress;

namespace {
QString validServiceId()
{
    QByteArray publicKey(32, Qt::Uninitialized);
    for (int i = 0; i < publicKey.size(); ++i) {
        publicKey[i] = static_cast<char>(i);
    }
    QByteArray checksumInput(".onion checksum", 15);
    checksumInput += publicKey;
    checksumInput += static_cast<char>(3);
    const auto digest = QCryptographicHash::hash(checksumInput, QCryptographicHash::Sha3_256);
    return ds::crypto::onionv3encode(publicKey + digest.left(2) + QByteArray(1, 3));
}
}

class TorAddressTest final : public QObject
{
    Q_OBJECT

private slots:
    void parsesCanonicalAndDisplayForms()
    {
        const auto serviceId = validServiceId();
        QVERIFY(TorAddress::isValidServiceId(serviceId));

        const auto canonical = serviceId + ".onion:443";
        const auto parsed = TorAddress::parse(canonical);
        QCOMPARE(parsed.service_id, serviceId);
        QCOMPARE(parsed.port, static_cast<quint16>(443));
        QCOMPARE(parsed.host(), serviceId + ".onion");
        QCOMPARE(parsed.toString(), canonical);

        const auto prefixed = TorAddress::parse("onion:" + serviceId + ":443");
        QCOMPARE(prefixed.toString(), canonical);
    }

    void rejectsLegacyAndMalformedAddresses()
    {
        QVERIFY(!TorAddress::isValidServiceId(QString(56, QLatin1Char('a'))));
        QVERIFY_EXCEPTION_THROWN(TorAddress::parse("abcdefghijklmnop.onion:443"), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(TorAddress::parse(validServiceId() + ".onion:0"), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(TorAddress::parse(validServiceId() + ".onion:65536"), std::invalid_argument);

        auto invalid = validServiceId();
        invalid[10] = invalid[10] == 'a' ? 'b' : 'a';
        QVERIFY(!TorAddress::isValidServiceId(invalid));
    }
};

QTEST_MAIN(TorAddressTest)
#include "tor_address_test.moc"
