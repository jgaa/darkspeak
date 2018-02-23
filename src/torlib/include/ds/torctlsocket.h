#ifndef TORCTLSOCKET_H
#define TORCTLSOCKET_H

#include <functional>
#include <deque>
#include <map>
#include <string.h>
#include <algorithm>
#include <QQueue>
#include <QObject>
#include <QTcpSocket>

namespace ds {
namespace tor {

struct TorCtlReply {
    int status = {};
    std::deque<std::string> lines;

    struct ParseError : public std::runtime_error
    {
        ParseError(const char *what) : std::runtime_error(what) {}
    };

    static std::string tolower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower );
        return s;
    }

    // https://stackoverflow.com/questions/19102195/how-to-make-stlmap-key-case-insensitive
    struct cmp {
        bool operator() (const std::string& lhs, const std::string& rhs) const {
            return  tolower(lhs) < tolower(rhs);
        }
    };

    using map_t = std::map<std::string, QVariant, cmp>;
    using submap_t = QMap<QString, QVariant>;

    // Parse the raw reply into key/value pairs.
    // The QVariant value may be a new map with key/value pairs in the form
    // QMap<QByteArray, QByteArray>
    map_t parse() const;

    // Try to parse a line into a key/value map. Return false if this is not a key/value set
    bool parse(const std::string& data, submap_t& kv) const;

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
    void sendCommand(QByteArray command, handler_t handler);

signals:
    // If triggered, the connection is dead
    void error(const QString &message);
    void gotReply(const TorCtlReply& reply);

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
    TorCtlReply current_reply_;
    State state_ = State::READY;
};

}} // namespaces

Q_DECLARE_METATYPE(ds::tor::TorCtlReply);

#endif // TORCTLSOCKET_H
