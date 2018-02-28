#ifndef PROTOCOLMANAGER_H
#define PROTOCOLMANAGER_H

#include <QObject>

namespace ds {

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
    ProtocolManager();

signals:
    void incomingMessage(const Message&);
    void messageReport(const MessageReport&);

public slots:
    virtual void sendMessage(const Message&) = 0;
};

} // namepsace


#endif // PROTOCOLMANAGER_H
