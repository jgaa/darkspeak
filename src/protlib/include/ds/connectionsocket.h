#ifndef CONNECTIONSOCKET_H
#define CONNECTIONSOCKET_H

#include <memory>

#include <QTcpSocket>
#include <QUuid>

namespace ds {
namespace prot {

class ConnectionSocket : public QTcpSocket
{
    Q_OBJECT
public:
    using ptr_t = std::shared_ptr<ConnectionSocket>;

    ConnectionSocket();

    const QUuid& getUuid() const noexcept {
        return uuid;
    }

signals:
    void connectedToHost(const QUuid& uuid);
    void socketFailed(const QUuid& uuid, const SocketError& socketError);
    void disconnectedFromHost(const QUuid& uuid);

private slots:
    void onConnected();
    void onDisconnected();
    void onSocketFailed(const SocketError& socketError);

private:
    QUuid uuid = QUuid::createUuid();

};

}} // namespaces

#endif // CONNECTIONSOCKET_H
