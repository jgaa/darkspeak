#ifndef PROTOCOLMANAGER_H
#define PROTOCOLMANAGER_H

#include <memory>

#include <QObject>
#include <QSettings>

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

public:
    static ptr_t create(QSettings& settings, Transport transport);
};

}} // namepsace


#endif // PROTOCOLMANAGER_H
