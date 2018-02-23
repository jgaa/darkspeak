#include "tst_torctlsocket.h"

TestTorCtlSocket::TestTorCtlSocket()
{
    qRegisterMetaType<ds::tor::TorCtlReply>("TorCtlReply");
    qRegisterMetaType<ds::tor::TorCtlReply>("::ds::tor::TorCtlReply");
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
    ctl.sendCommand("PROTOCOLINFO 1", [&](const ds::tor::TorCtlReply& reply) {
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

    QSignalSpy spy_reply(&ctl, SIGNAL(gotReply(::ds::tor::TorCtlReply)));
    ctl.sendCommand("PROTOCOLINFO 1", [&](const ::ds::tor::TorCtlReply& reply) {
        QVERIFY(reply.status == 250);
    });
    QCOMPARE(spy_reply.wait(1000), true);
}

void TestTorCtlSocket::test_reply_parser_1()
{
    ds::tor::TorCtlReply reply;

    reply.lines.push_back(R"(Version "1.2.3")");
    auto map = reply.parse();

    QVERIFY(map.size() == 1);
    QCOMPARE(QString("1.2.3"), map["version"].toString());
}

void TestTorCtlSocket::test_reply_parser_unescape()
{
    ds::tor::TorCtlReply reply;

    QCOMPARE(std::string("abc\r\n\\\t"), reply.unescape(R"("abc\r\n\\\t")"));
    QCOMPARE(std::string("abc\1a\2888\002"), reply.unescape(R"("abc\1a\2888\002")"));
    QCOMPARE(std::string("abc\123"), reply.unescape(R"("abc\123")"));
    QCOMPARE(std::string("abc\12"), reply.unescape(R"("abc\12")"));
    QCOMPARE(std::string("abc\1"), reply.unescape(R"("abc\1")"));

    {
        auto fun = [&] {reply.unescape(R"("abc)");};
        QVERIFY_EXCEPTION_THROWN(fun(), ::ds::tor::TorCtlReply::ParseError);
    }

    {
        auto fun = [&] {reply.unescape(R"("abc\")");};
        QVERIFY_EXCEPTION_THROWN(fun(), ::ds::tor::TorCtlReply::ParseError);
    }

    {
        auto fun = [&] {reply.unescape(R"("abc\\\")");};
        QVERIFY_EXCEPTION_THROWN(fun(), ::ds::tor::TorCtlReply::ParseError);
    }
}

void TestTorCtlSocket::test_reply_map_parser()
{
    ds::tor::TorCtlReply reply;

    reply.lines = {"PROTOCOLINFO 1",
                   "AUTH METHODS=COOKIE,SAFECOOKIE COOKIEFILE=\"/var/run/tor/control.authcookie\"",
                   "VERSION Tor=\"0.2.9.14\"",
                   "OK"};
    const auto map = reply.parse();
    QCOMPARE(QString("1"), map.at("PROTOCOLINFO").toString());
    QCOMPARE(QString("0.2.9.14"), map.at("VERSION").toMap().value("TOR").toString());
    QCOMPARE(QString("COOKIE,SAFECOOKIE"), map.at("AUTH").toMap().value("METHODS").toString());
    QCOMPARE(QString("/var/run/tor/control.authcookie"), map.at("AUTH").toMap().value("COOKIEFILE").toString());
    QCOMPARE(static_cast<size_t>(4), map.size());
}


