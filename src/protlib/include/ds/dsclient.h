#ifndef DSCLIENT_H
#define DSCLIENT_H

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
    enum class State {
        CONNECTED
    };

    DsClient(ConnectionSocket::ptr_t connection);

    ConnectionSocket& getConnection() {
        if (!connection_) {
            throw std::runtime_error("No connection object");
        }
        return *connection_;
    }

private slots:
    void advance();

private:
    void sayHello();

    ConnectionSocket::ptr_t connection_;
    State state_ = State::CONNECTED;
};

}} // namespaces

#endif // DSCLIENT_H
