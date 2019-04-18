#ifndef TORCTLSOCKET_H
#define TORCTLSOCKET_H

#include <functional>
#include <deque>
//#include <map>
#include <locale>
#include <string.h>
#include <algorithm>
#include <QQueue>
#include <QObject>
#include <QTcpSocket>

namespace ds {
namespace tor {

template<typename T>
QString toKey(const T& key) {
    QString rval;

    std::locale loc;
    for(const auto ch : key) {
        rval += std::toupper(ch);
    }

    return rval;
}

struct TorCtlReply {
    std::deque<std::string> lines;
    int status = {};

    struct ParseError : public std::runtime_error
    {
        ParseError(const char *what) : std::runtime_error(what) {}
    };

    using map_t = QMap<QString, QVariant>;

    // Parse the raw reply into key/value pairs.
    // The QVariant value may be a new map with key/value pairs in the form
    // QMap<QByteArray, QByteArray>
    map_t parse() const;

    // Try to parse a line into a key/value map. Return false if this is not a key/value set
    bool parse(const std::string& data, map_t& kv) const;

    // Unescape escaped (double quoted) section(s) of a string
    static std::string unescape(const std::string& escaped);

    // Can parse a string and unescape quoted section(s) = used = nullptr,
    // or can parse a quoted string from the leading double quote, and report
    // the consumed bytes, including the ending double quote in used.
    static std::string unescape(const std::string::const_iterator start,
                                const std::string::const_iterator end,
                                size_t *used = nullptr);
};


class TorCtlSocket : public QTcpSocket
{
    Q_OBJECT

    enum State {
        READY,
        IN_REPLY,
        IN_DATA
    };

public:
    struct IoError : public std::runtime_error
    {
        IoError(const char *what) : std::runtime_error(what) {}
    };

    using handler_t = std::function<void (const TorCtlReply&)>;

    explicit TorCtlSocket();

    /*! Send a command to the TOR server
     *
     * \param command Command to send. Does not have to be terminated with CRLF
     * \param handler Handler to deal with the result of the operation. May be empty.
     * \exception IoError is write fails.
    */
    void sendCommand(QByteArray command, const handler_t& handler);

signals:
    // If triggered, the connection is dead
    void error(const QString &message);
    void torReply(const TorCtlReply& reply);
    void torEvent(const TorCtlReply& reply);

protected slots:
    void processIn();
    void clear();

protected:
    virtual bool canReadLine_() const { return canReadLine(); }
    virtual qint64 write_(const QByteArray& data) { return write(data); }
    virtual QByteArray readLine_(qint64 maxLen) {return readLine(maxLen); }

private:
    void setError(const QString& error);

    QQueue<handler_t> pending_;
    constexpr static size_t max_buffer_len_ = 1024 * 5;
    constexpr static size_t max_reply_lines_ = 32;
    constexpr static int max_data_in_one_reply_line_ = 1024 * 16;
    TorCtlReply current_reply_;
    State state_ = State::READY;
};

}} // namespaces

Q_DECLARE_METATYPE(ds::tor::TorCtlReply);

#endif // TORCTLSOCKET_H
