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
        OFFLINE,
        ONLINE
    };

    enum class Transport {
        TOR
    };

    ProtocolManager();
    virtual ~ProtocolManager() = default;

    virtual State getState() const = 0;
    virtual bool isOnline() const { return getState() == State::ONLINE; }

signals:
    /*! The protocol is running, but may still not be on-line */
    void started();

    /*! The protocol is running and on-line */
    void online();

    /*! protocol is running but off-line */
    void offline();

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
