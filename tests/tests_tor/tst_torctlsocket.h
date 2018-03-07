#ifndef TST_TORCTLSOCKET_H
#define TST_TORCTLSOCKET_H

#include <QtTest>
#include <assert.h>

#include "ds/torctlsocket.h"

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
    void test_reply_parser_1();
    void test_reply_parser_unescape();
    void test_reply_map_parser();

};


#endif // TST_TORCTLSOCKET_H
