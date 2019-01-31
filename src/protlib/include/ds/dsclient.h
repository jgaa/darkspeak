#ifndef DSCLIENT_H
#define DSCLIENT_H

#include <memory>

#include <sodium.h>

#include "ds/protocolmanager.h"
#include "ds/connectionsocket.h"


namespace ds {
namespace prot {

/*! Client implementation of the DarkSpeak protocol
 *
 */
class DsClient : public QObject
{
    Q_OBJECT
public:
    using ptr_t = std::shared_ptr<DsClient>;

    enum class State {
        CONNECTED,
        GET_SERVER_HELLO,
    };

    DsClient(ConnectionSocket::ptr_t connection, core::ConnectData connectionData);

    ConnectionSocket& getConnection() {
        if (!connection_) {
            throw std::runtime_error("No connection object");
        }
        return *connection_;
    }

    ConnectionSocket::ptr_t getConnectionPtr() {
        if (!connection_) {
            throw std::runtime_error("No connection object");
        }
        return connection_;
    }

private slots:
    void advance();

private:
    void sayHello();
    void getHelloReply();

    ConnectionSocket::ptr_t connection_;
    State state_ = State::CONNECTED;
    core::ConnectData connectionData_;
    crypto_secretstream_xchacha20poly1305_state stateOut;
};

}} // namespaces

#endif // DSCLIENT_H
