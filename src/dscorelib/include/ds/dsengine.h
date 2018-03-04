#ifndef DSENGINE_H
#define DSENGINE_H

#include <memory>

#include <QObject>
#include <QSettings>

#include "ds/database.h"

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

public:
    enum State {
        INITIALIZING,
        RUNNING,
        CLOSING,
        TERMINATED
    };

    DsEngine();
    DsEngine(std::unique_ptr<QSettings> settings);
    ~DsEngine();


    static DsEngine& instance();
    State getState() const;

    QSettings& settings() noexcept { return *settings_; }

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

protected:
    void initialize();

    std::unique_ptr<QSettings> settings_;
    std::unique_ptr<Database> database_;
    static DsEngine *instance_;
};

}} // namepsaces

#endif // DSENGINE_H
