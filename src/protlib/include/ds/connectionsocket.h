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

    template <typename T>
    void write(const T& data) {
        const char *p = reinterpret_cast<const char *>(data.data());
        const qint64 bytes = data.size();
        outData.append(p, bytes);
        auto written = QTcpSocket::write(outData);
        if (written > 0) {
            outData.remove(0, static_cast<int>(written));
        }
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
    QByteArray outData;

};

}} // namespaces


Q_DECLARE_SMART_POINTER_METATYPE(std::shared_ptr)
Q_DECLARE_METATYPE(ds::prot::ConnectionSocket::ptr_t)

#endif // CONNECTIONSOCKET_H
