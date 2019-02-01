#ifndef PROTOCOLMANAGER_H
#define PROTOCOLMANAGER_H

#include <memory>

#include <QUuid>
#include <QObject>
#include <QSettings>
#include <QAbstractSocket>

#include "ds/transporthandle.h"
#include "ds/dscert.h"

namespace ds {
namespace core {

class Message;
class MessageReport;

struct ConnectData {
    QUuid service; // for Identity
    QByteArray address;  // Onion address
    QByteArray contactsPubkey;
    crypto::DsCert::ptr_t identitysCert;
};

/*! Generic interface to the IM protocol.
 *
 * Currently we only support Tor as a transport layer,
 * but this may change in the future.
 *
 * A service is identified from it's certificates hash.
 */
class ProtocolManager : public QObject
{
        Q_OBJECT
public:
    using ptr_t = std::shared_ptr<ProtocolManager>;

    enum State {
        // Not connected to anything
        OFFLINE,

        // Connecting to transport (Tor server)
        CONNECTING,

        // Connected to transport. Can create identities
        CONNECTED,

        // Online. Can communicate with peers
        ONLINE,

        // In the process of shutting down
        SHUTTINGDOWN
    };

    enum class Transport {
        TOR
    };

    ProtocolManager();
    virtual ~ProtocolManager() = default;

    virtual State getState() const = 0;
    virtual bool isOnline() const { return getState() == State::ONLINE; }

signals:
    /*! We are trying to connect to the transport */
    void connecting();

    /*! The protocol is running, but may still not be on-line */
    void connected();

    /*! The protocol is running and on-line */
    void online();

    /*! protocol is running but transport reported that it is not on-line anymore */
    void offline();

    /*! We are shutting down */
    void shutdown();

    /*! Incoming message */
    void incomingMessage(const Message&);

    /*! Incomming message delivery report */
    void messageReport(const MessageReport&);

    void stateChanged(const State old, const State current);

    void transportHandleReady(const TransportHandle&);
    void transportHandleError(const TransportHandleError&);

    void serviceFailed(const QUuid& service, const QByteArray& reason);
    void serviceStarted(const QUuid& service, const bool newService);
    void serviceStopped(const QUuid& service);

    void connectedTo(const QUuid& connection);
    void disconnectedFrom(const QUuid& connection);
    void connectionFailed(const QUuid& connection,
                          const QAbstractSocket::SocketError& socketError);

public slots:

    /*! Start the protocol */
    virtual void start() = 0;

    /*! Stop the protocol
     *
     * This stops all hidden servics and disconnecrts from the Tor service.
     */
    virtual void stop() = 0;

    /*! Send a message.
     *
     * Throws if the protocol is not on-line
     */
    virtual void sendMessage(const Message&) = 0;

    /*! Create a hidden service for an identity */
    virtual void createTransportHandle(const TransportHandleReq&) = 0;

    /*! Start a service for an identity
     *
     * \param handle Data reqired to start a communication service.
     *
     * Signals emitted later: serviceStarted or serviceFailed.
     */
    virtual void startService(const QUuid& service,
                              const crypto::DsCert::ptr_t& cert,
                              const QVariantMap& data) = 0;

    /*! Stop a service for an identity
     *
     * \param id The same id that was provided when the service was started.
     *
     * Signals emitted later: serviceStopped or serviceFailed.
     */
    virtual void stopService(const QUuid& service) = 0;

    /*! Create a connection to a contact */
    virtual QUuid connectTo(ConnectData cd) = 0;

    /*! Close or cancel a connection to a contact
     *
     * When the method returns, the uuid is no longer valid.
     */
    virtual void disconnectFrom(const QUuid& service,
                                const QUuid& connection) = 0;


public:
    static ptr_t create(QSettings& settings, Transport transport);
};

}} // namepsace


#endif // PROTOCOLMANAGER_H
