#include <QtTest>

#include "ds/torconfig.h"

using ds::tor::TorConfig;

class TorConfigTest final : public QObject
{
    Q_OBJECT

private slots:
    void embeddedIsTheDefaultMode()
    {
        QCOMPARE(TorConfig{}.mode, TorConfig::Mode::PRIVATE);
    }

    void privateArgumentsUsePrivateEndpointsAndCookieAuth()
    {
        const auto args = TorConfig::privateArguments(
            "/tmp/darkspeak-tor/data",
            "/tmp/darkspeak-tor/control.sock",
            "/tmp/darkspeak-tor/socks.sock",
            43127);

        QVERIFY(args.contains("--DataDirectory"));
        QVERIFY(args.contains("/tmp/darkspeak-tor/data"));
        QVERIFY(args.contains("unix:/tmp/darkspeak-tor/control.sock"));
        QVERIFY(args.contains("unix:/tmp/darkspeak-tor/socks.sock"));
        QVERIFY(args.contains("127.0.0.1:43127"));
        QVERIFY(args.contains("--CookieAuthentication"));
        QVERIFY(args.contains("1"));
        QVERIFY(args.contains("--ControlSocketsGroupWritable"));
        QVERIFY(args.contains("--UnixSocksGroupWritable"));
        QVERIFY(!args.contains("127.0.0.1:9050"));
        QVERIFY(!args.contains("127.0.0.1:9051"));
    }
};

QTEST_MAIN(TorConfigTest)
#include "tor_config_test.moc"
