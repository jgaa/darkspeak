#include "torctlsocket.h"

namespace ds {
namespace tor {


TorCtlSocket::TorCtlSocket()
{
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

            current_reply_ = Reply{};
            current_reply_.status = line.left(3).toInt();
        }

        if (state_ == State::IN_DATA) {
            if (line == ".") {
                state_ = State::IN_REPLY;
            } else {
                current_reply_.lines.back() += line.mid(4);
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

        current_reply_.lines.push_back(line.mid(4));

        if (line_type == '+') {
            state_ = State::IN_DATA;
            continue;
        }

        if (line_type != ' ') {
            continue; // Not finished quite yet
        }

        // At this point we have the final line in the reply.
        if (current_reply_.status >= 600 && current_reply_.status < 700) {

            // Asynchronous response. We have not requested that, so let's
            // just ignore it for now!

            qDebug() << "Unexpected async message: "
                     << current_reply_.status << ' '
                     << current_reply_.lines.front();

            continue;
        }

        if (pending_.empty()) {
            qWarning() << "Received orphan response from tor: "
                       << current_reply_.status << ' '
                       << current_reply_.lines.front();
            continue;
        }

        const auto handler = pending_.takeFirst();
        try {
            if (handler) {
                handler(current_reply_);
            }
            emit gotReply(current_reply_);
        } catch(const std::exception& ex) {
            qWarning() << "Caught exeption from handler: " << ex.what();
        }

        state_ = State::READY;
    }
}

void TorCtlSocket::clear()
{
    qDebug() << "TorCtlSocket is disconnected";
}

void TorCtlSocket::setError(QString errorMsg)
{
    qWarning() << "Error on Tor Conrol channel: " << errorMsg;
    emit error(errorMsg);
    abort();
}

}} // namespaces
