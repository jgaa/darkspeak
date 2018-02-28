#ifndef DSENGINE_H
#define DSENGINE_H

#include <QObject>

namespace ds {
namespace core {

class Identity;
class IdentityReq;
class IdentityError;
class TransportHandle;
class TransportHandleReq;
class TransportHandleError;

/*! The core engine of DarkSpeak.
 *
 * It owns the database, and it acts like a layer
 * between the QT Models above and the actual protocol
 * below.
 *
 * There is one instance of this object in a DarkSpeak application.
 */
class DsEngine : public QObject
{
    Q_OBJECT
    DsEngine();

public:
    enum State {
        INITIALIZING,
        RUNNING,
        CLOSING,
        TERMINATED
    };

    DsEngine& instance();
    State getState() const;

public slots:
    void createIdentity(const IdentityReq&);
    void createTransportHandle(const TransportHandleReq&);
    void close();

signals:
    void identityCreated(const Identity&);
    void identityError(const IdentityError&);
    void transportHandleReady(const TransportHandle&);
    void transportHandleError(const TransportHandleError&);
    void closing();
    void changedState(const State);

};

}} // namepsaces

#endif // DSENGINE_H
