#include <QtTest>

// add necessary includes here
#include <assert.h>

//#include "ds/tormgr.h"
#include "torctlsocket.h"

class TestTorCtlSocket : public QObject
{
    Q_OBJECT

    class MockTorCtlSocket : public ds::tor::TorCtlSocket
    {
    public:
        MockTorCtlSocket(std::vector<std::string> reply)
            : reply_{move(reply)} {}

        bool canReadLine_() const override {
            return !reply_.empty();
        }

        QByteArray readLine_(qint64 maxLen) override {
            if (reply_.empty()) {
                return 0;
            }

            assert(maxLen > static_cast<qint64>(reply_.front().size()));

            auto line = reply_.front();
            reply_.erase(reply_.begin());
            return line.c_str();
        }

        qint64 write_(const QByteArray& data) override { return data.size(); }

        void mockReceiving() {
            processIn();
        }

    private:
        std::vector<std::string> reply_;
    };

public:
    TestTorCtlSocket();
    ~TestTorCtlSocket();

private slots:
    void test_mock_protocolinfo();
    void test_protocolinfo();

};

TestTorCtlSocket::TestTorCtlSocket()
{
    qRegisterMetaType<ds::tor::TorCtlSocket::Reply>("Reply");
}

TestTorCtlSocket::~TestTorCtlSocket()
{

}

void TestTorCtlSocket::test_mock_protocolinfo()
{
    MockTorCtlSocket ctl({"250-PROTOCOLINFO 1\r\n",
                          "250-AUTH METHODS=COOKIE,SAFECOOKIE COOKIEFILE=\"/var/run/tor/control.authcookie\"\r\n",
                          "250-VERSION Tor=\"0.2.9.14\"\r\n",
                          "250 OK\r\n"});
    bool in_lambda = false;
    ctl.sendCommand("PROTOCOLINFO 1", [&](const ds::tor::TorCtlSocket::Reply& reply) {
        QVERIFY(reply.status == 250);
        in_lambda = true;
    });
    ctl.mockReceiving();
    QVERIFY(in_lambda);
}

// Here we need an actual tor server running on localhost:9051
void TestTorCtlSocket::test_protocolinfo()
{
    ds::tor::TorCtlSocket ctl;
    ctl.connectToHost("localhost", 9051);
    QSignalSpy spy_connect(&ctl, SIGNAL(connected()));
    QCOMPARE(spy_connect.wait(1000), true);

    QSignalSpy spy_reply(&ctl, SIGNAL(gotReply(Reply)));
    ctl.sendCommand("PROTOCOLINFO 1", [&](const ds::tor::TorCtlSocket::Reply& reply) {
        QVERIFY(reply.status == 250);
    });
    QCOMPARE(spy_reply.wait(1000), true);
}

QTEST_GUILESS_MAIN(TestTorCtlSocket)

#include "tst_torctlsocket.moc"
