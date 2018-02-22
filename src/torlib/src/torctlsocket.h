#ifndef TORCTLSOCKET_H
#define TORCTLSOCKET_H

#include <functional>
#include <deque>
#include <QQueue>
#include <QObject>
#include <QTcpSocket>

namespace ds {
namespace tor {

class TorCtlSocket : public QTcpSocket
{
    Q_OBJECT

    enum State {
        READY,
        IN_REPLY,
        IN_DATA
    };

public:
    struct Reply {
        int status = {};
        std::deque<QByteArray> lines;
    };

    struct IoError : public std::runtime_error
    {
        IoError(const char *what) : std::runtime_error(what) {}
    };

    using handler_t = std::function<void (const Reply&)>;

    explicit TorCtlSocket();

    /*! Send a command to the TOR server
     *
     * \param command Command to send. Does not have to be terminated with CRLF
     * \param handler Handler to deal with the result of the operation. May be empty.
     * \exception IoError is write fails.
    */
    void sendCommand(QByteArray command, handler_t handler);

signals:
    // If triggered, the connection is dead
    void error(const QString &message);
    void gotReply(const Reply& reply);

protected slots:
    void processIn();
    void clear();

protected:
    virtual bool canReadLine_() const { return canReadLine(); }
    virtual qint64 write_(const QByteArray& data) { return write(data); }
    virtual QByteArray readLine_(qint64 maxLen) {return readLine(maxLen); }

private:
    void setError(QString error);

    QQueue<handler_t> pending_;
    constexpr static size_t max_buffer_len_ = 1024 * 5;
    constexpr static size_t max_reply_lines_ = 32;
    constexpr static int max_data_in_one_reply_line_ = 1024 * 16;
    Reply current_reply_;
    State state_ = State::READY;
};

}} // namespaces

#endif // TORCTLSOCKET_H
