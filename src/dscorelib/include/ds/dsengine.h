#ifndef DSENGINE_H
#define DSENGINE_H

#include <memory>

#include <QObject>
#include <QSettings>

#include "ds/database.h"
#include "ds/identity.h"
#include "ds/transporthandle.h"
#include "ds/dscert.h"
#include "ds/protocolmanager.h"

namespace ds {
namespace core {

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
        STARTING,
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
    ProtocolManager& getProtocolMgr(ProtocolManager::Transport transport);
    static const QByteArray& getName(const State state);
    bool isOnline() const;

public slots:
    void createIdentity(const IdentityReq&);
    void close();
    void start();

private slots:
    void onCertCreated(const QString& name, const ds::crypto::DsCert::ptr_t cert);
    void addIdentityIfReady(const QString& name);
    void whenOnline(std::function<void ()> fn);
    void online();
    void onTransportHandleReady(const TransportHandle& th);
    void onTransportHandleError(const TransportHandleError& th);

signals:
    void identityCreated(const Identity&);
    void identityError(const IdentityError&);
    void ready();
    void closing();
    void changedState(const State from, const State to);
    void certCreated(const QString name, const ds::crypto::DsCert::ptr_t cert);
    void retryIdentityReady(const QString name); // internal

protected:
    void initialize();
    void setState(State state);
    void tryMakeTransport(const QString& name);

    std::unique_ptr<QSettings> settings_;
    std::unique_ptr<Database> database_;
    static DsEngine *instance_;
    QMap<QString, Identity> pending_identities_;
    ProtocolManager::ptr_t tor_mgr_;
    State state_ = State::INITIALIZING;
    QList<std::function<void ()>> when_online_;
};

}} // namepsaces

#endif // DSENGINE_H
