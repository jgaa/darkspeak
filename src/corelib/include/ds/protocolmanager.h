#ifndef PROTOCOLMANAGER_H
#define PROTOCOLMANAGER_H

#include <memory>

#include <QUuid>
#include <QObject>
#include <QSettings>
#include <QAbstractSocket>

#include "ds/transporthandle.h"

namespace ds {
namespace core {

class Message;
class MessageReport;

/*! Generic interface to the IM protocol.
 *
 * Currently we only support Tor as a transport layer,
 * but this may change in the future.
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

    void serviceFailed(const QByteArray& id, const QByteArray& reason);
    void serviceStarted(const QByteArray& id);
    void serviceStopped(const QByteArray& id);

    void connectedTo(const QByteArray& serviceId, const QUuid& uuid);
    void disconnectedFrom(const QByteArray& serviceId, const QUuid& uuid);
    void connectionFailed(const QByteArray& serviceId,
                          const QUuid& uuid,
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
     * \param id Persistent, unique identifier for the Identity and the service
     *      This is an opaque argument for the engine, but status changes will
     *      be identified by this id, so the caller must be able to map it to
     *      an Identity.
     * \param handle Data reqired to start a communication service.
     *
     * Signals emitted later: serviceStarted or serviceFailed.
     */
    virtual void startService(const QByteArray& id, const QVariantMap& data) = 0;

    /*! Stop a service for an identity
     *
     * \param id The same id that was provided when the service was started.
     *
     * Signals emitted later: serviceStopped or serviceFailed.
     */
    virtual void stopService(const QByteArray& id) = 0;

    /*! Create a connection to a contact */
    virtual QUuid connectTo(const QByteArray& serviceId,
                            const QByteArray& address) = 0;

    /*! Close or cancel a connection to a contact
     *
     * When the method returns, the uuid is no longer valid.
     */
    virtual void disconnectFrom(const QByteArray& serviceId,
                                const QUuid& uuid) = 0;


public:
    static ptr_t create(QSettings& settings, Transport transport);
};

}} // namepsace


#endif // PROTOCOLMANAGER_H
