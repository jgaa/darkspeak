
#include <regex>
#include <assert.h>
#include <locale>

#include "logfault/logfault.h"
#include "ds/torctlsocket.h"

namespace ds {
namespace tor {


TorCtlSocket::TorCtlSocket()
{
    static bool registered = false;
    if (!registered) {
        registered = true;
        qRegisterMetaType<ds::tor::TorCtlReply>("TorCtlReply");
        qRegisterMetaType<ds::tor::TorCtlReply>("::ds::tor::TorCtlReply");
    }

    connect(this, SIGNAL(readyRead()), this, SLOT(processIn()));
    connect(this, SIGNAL(disconnected()), this, SLOT(clear()));
}

void TorCtlSocket::sendCommand(QByteArray command,
                               TorCtlSocket::handler_t handler)
{
    if (!command.endsWith("\r\n")) {
        command += "\r\n";
    }

    if (write_(command) != command.size()) {
        static const auto err = "Failed to write to the Tor command socket";
        setError(err);
        throw IoError(err);
    }
    pending_.push_back(move(handler));
}

void TorCtlSocket::processIn()
{
    while(canReadLine_()) {
        // Reply format: nnn SP|+|- text CRLF

        QByteArray line = readLine_(max_buffer_len_);
        if (!line.endsWith("\r\n")) {
            setError(QStringLiteral("Invalid control reply syntax: Missing CRLF"));
            return;
        }

        line.chop(2);

        if (line.size() < 4) {
            setError(QStringLiteral("Invalid control reply syntax: Too short."));
            return;
        }

        const char line_type = line[3];

        if (line_type != ' ' && line_type != '-' && line_type != '+') {
            setError(QStringLiteral("Invalid control reply syntax: Invalid character after result code."));
            return;
        }

        if (state_ == State::READY) {
            state_ = State::IN_REPLY;

            current_reply_ = TorCtlReply{};
            current_reply_.status = line.left(3).toInt();
        }

        if (state_ == State::IN_DATA) {
            if (line == ".") {
                state_ = State::IN_REPLY;
            } else {
                current_reply_.lines.back() += line.mid(4).toStdString();
                if (current_reply_.lines.back().size()
                        > max_data_in_one_reply_line_) {
                    setError(QStringLiteral("Invalid control reply syntax: Too verbose."));
                    return;
                }
            }
            continue;
        }

        Q_ASSERT(state_ == State::IN_REPLY);

        if (current_reply_.lines.size() > max_reply_lines_) {
            setError(QStringLiteral("Invalid control reply syntax: Too much babble."));
            return;
        }

        current_reply_.lines.push_back(line.mid(4).toStdString());

        if (line_type == '+') {
            state_ = State::IN_DATA;
            continue;
        }

        if (line_type != ' ') {
            continue; // Not finished quite yet
        }

        // At this point we have the final line in the reply.
        if (current_reply_.status >= 600 && current_reply_.status < 700) {

            // Asynchronous response.
            LFLOG_DEBUG << "Torctl received event: "
                     << current_reply_.status << ' '
                     << current_reply_.lines.front().c_str();
            emit torEvent(current_reply_);
            state_ = State::READY;
            continue;
        }

        if (pending_.empty()) {
            qWarning() << "Received orphan response from tor: "
                       << current_reply_.status << ' '
                       << current_reply_.lines.front().c_str();
            continue;
        }

        const auto handler = pending_.takeFirst();
        try {
            LFLOG_DEBUG << "Torctl received reply: "
                     << current_reply_.status << ' '
                     << current_reply_.lines.front().c_str();
            if (handler) {
                handler(current_reply_);
            }
            emit torReply(current_reply_);
        } catch(const std::exception& ex) {
            qWarning() << "Caught exeption from handler: " << ex.what();
            qWarning() << "Shutting down connection to torctl!";
            close();
        }

        state_ = State::READY;
    }
}

void TorCtlSocket::clear()
{
    LFLOG_DEBUG << "TorCtlSocket is disconnected";
}

void TorCtlSocket::setError(QString errorMsg)
{
    qWarning() << "Error on Tor Conrol channel: " << errorMsg;
    emit error(errorMsg);
    abort();
}

TorCtlReply::map_t TorCtlReply::parse() const
{
    static const std::regex line_breakup(R"(^(([\w][\w-\/]+)(=(\w+)) )?(\w+)( (.*))?$)");
    map_t rval;

    for(const auto& line: lines) {
        // Get the first word
        std::smatch m;
        if (std::regex_match(line, m, line_breakup)) {
            assert(m.size() == 8);
            const std::string level = m[4].str();
            const std::string key = m[5].str();
            const std::string value = m[7].str();

            map_t kv;

            if (!level.empty()) {
                rval[toKey(m[2].str())] = level.c_str();
            }

            if (parse(value, kv)) {
                rval[toKey(key)] = QVariant(kv);
            } else {
                auto escaped_value = unescape(value);
                rval[toKey(key)] = QString(escaped_value.c_str());
            }
        } else {
            parse(line, rval);
        }
    }

    return rval;
}

bool TorCtlReply::parse(const std::string &data, TorCtlReply::map_t &kv) const
{
    std::locale loc;
    enum class State {
        SCANNING,
        KEY,
        VALUE
    };

    State state = State::SCANNING;
    QByteArray key;
    QByteArray value;

    for(auto it = data.begin(); it != data.end(); ++it) {
        if (state == State::SCANNING) {
            if (*it == ' ' || *it == '\t') {
                continue; // Skip whitespace
            }

            if (std::isalpha(*it, loc)) {
                state = State::KEY;
                assert(key.isEmpty());
                assert(value.isEmpty());
            } else {
                return false; // Not a strict key=value ... input
            }
        }

        if (state == State::KEY) {
            if (*it == '=') {
                state = State::VALUE;
                continue;
            }

            key += *it;
        }

        if (state == State::VALUE) {
            if (*it == '\"') {
                size_t used = 0;
                auto escaped = unescape(it, data.end(), &used);
                value += escaped.c_str();
                assert(used > 0);

                // we want to wrap past doublequote in the loop, not here
                it += static_cast<int>(used) - 1;

                // We assume that a key="..." only contain the quoted value
                state = State::SCANNING;
            } else if (*it == ' ' || *it == '\t') {
                state = State::SCANNING;
            } else {
                value += *it;
            }
        }

        if (state == State::SCANNING || (it + 1) == data.end()) {
            if (!key.isEmpty()) {
                kv[toKey(key)] = value;
            }

            key = "";
            value.clear();
        }
    }

    return !kv.isEmpty();
}

/**
 *  Unescape value. Per https://spec.torproject.org/control-spec section 2.1.1:
 *
 *  For future-proofing, controller implementors MAY use the following
 *  rules to be compatible with buggy Tor implementations and with
 *  future ones that implement the spec as intended:
 *
 *  Read \n \t \r and \0 ... \377 as C escapes.
 *  Treat a backslash followed by any other character as that character.
 *
 * (Why is Tor, a security / anonymity thing, mandating complex parsing
 * that easily can lead to trivial errors and cause clients to blow up
 * if they connect to a Tor server that have exploints injected into it?)
 */

std::string TorCtlReply::unescape(const std::string::const_iterator start,
                                  const std::string::const_iterator end,
                                  size_t *used)
{
    std::string rval;
    rval.reserve(static_cast<size_t>(end - start));

    bool is_escaped = false;
    if (used) {
        *used = 0;
    }

    for(auto it = start; it != end; ++it) {
        if (is_escaped) {
            if (*it == '\\') {
                if (++it == end) {
                    // Invalid
                    throw ParseError("Quoted string ends with backslash");
                }

                if (*it == 'n') {
                    rval += '\n';
                } else if (*it == 'r') {
                    rval += '\r';
                } else if (*it == 't') {
                    rval += '\t';
                } else if (*it >= '0' && *it <= '7') {
                    // octal
                    int have_digits = 3;
                    std::string octet;
                    // Tor restricts first digit to 0-3 for three-digit octals.
                    // A leading digit of 4-7 would therefore be interpreted as
                    // a two-digit octal.
                    if (*it > '3') {
                        --have_digits;
                    }

                    do {
                        octet += *it;
                    } while (--have_digits
                             && (++it != end)
                             && (*it >= '0' && *it <= '7'));

                    if (have_digits) {
                        // Roll back one position
                        --it;
                    }

                    assert(!octet.empty());

                    auto ch = static_cast<char>(std::stoi(octet, nullptr, 8));
                    rval += ch;

                } else {
                    rval += *it;
                }

            } else if (*it == '\"') {
                is_escaped = false;
                if (used) {
                    *used = static_cast<size_t>(it - start) + 1;
                    return rval;
                }
            } else {
                rval += *it;
            }
        } else {
            // Looking for leading quote
            if (*it == '\"') {
                is_escaped = true;
            } else if (used) {
                throw ParseError("Quoted string must start with a double quote");
            } else {
                rval += *it;
            }
        }
    }

    if (is_escaped) {
        throw ParseError("Quoted string was unterminated");
    }

    return rval;
}

std::string TorCtlReply::unescape(const std::string &escaped)
{
    return unescape(escaped.cbegin(), escaped.cend());
}

}} // namespaces
