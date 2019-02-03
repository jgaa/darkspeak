#ifndef DSENGINE_H
#define DSENGINE_H

#include <memory>

#include <QObject>
#include <QSettings>
#include <QMap>

#include "ds/message.h"
#include "ds/database.h"
#include "ds/identity.h"
#include "ds/contact.h"
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
    QSqlDatabase& getDb();

    QSettings& settings() noexcept { return *settings_; }
    ProtocolManager& getProtocolMgr(ProtocolManager::Transport transport);
    static const QByteArray& getName(const State state);
    bool isOnline() const;
    static QByteArray getIdentityHandle(const QByteArray& pubkey, const QByteArray& address);
    static QByteArray toJson(const QVariantMap& data);
    static QVariantMap fromJson(const QByteArray& json);
    static QByteArray imageToBytes(const QImage& img);

public slots:
    void createIdentity(const IdentityReq&);
    void createContact(const ContactReq&);
    void createNewTransport(const QByteArray& name, const QUuid& uuid);
    void sendMessage(const Message& message);    
    void close();
    void start();

private slots:
    void onStateChanged(const ProtocolManager::State old, const ProtocolManager::State current);
    void onCertCreated(const QUuid& uuid, const ds::crypto::DsCert::ptr_t cert);
    void addIdentityIfReady(const QUuid& uuid);
    void whenOnline(std::function<void ()> fn);
    void onTransportHandleReady(const TransportHandle& th);
    void onTransportHandleError(const TransportHandleError& th);
    void online();
    void onServiceFailed(const QUuid& id, const QByteArray& reason);
    void onServiceStarted(const QUuid& id, const bool newService);
    void onServiceStopped(const QUuid& id);

signals:
    void identityCreated(const Identity&);
    void identityError(const IdentityError&);
    void contactCreated(const Contact& contact);
    void ready();
    void notReady(); // If the transport goes offline
    void closing();
    void stateChanged(const State from, const State to);
    void onlineStateChanged(const ProtocolManager::State old, const ProtocolManager::State current);
    void certCreated(const QUuid& uuid, const ds::crypto::DsCert::ptr_t cert);
    void retryIdentityReady(const QUuid& id); // internal
    void serviceFailed(const QUuid& uuid, const QByteArray& reason);
    void serviceStarted(const QUuid& uuid, const bool newService);
    void serviceStopped(const QUuid& uuid);
    void transportHandleReady(const TransportHandle& th);
    void transportHandleError(const TransportHandleError& th);
    void connectedTo(const QUuid& uuid);
    void disconnectedFrom(const QUuid& uuid);
    void connectionFailed(const QUuid& uuid,
                          const QAbstractSocket::SocketError& socketError);
    void incomingPeer(const QUuid& service, const QUuid& connectionId, const QByteArray& handle);

protected:
    void initialize();
    void setState(State state);
    void tryMakeTransport(const QString& name, const QUuid& uuid);

    std::unique_ptr<QSettings> settings_;
    std::unique_ptr<Database> database_;
    static DsEngine *instance_;
    QMap<QUuid, Identity> pending_identities_;
    ProtocolManager::ptr_t tor_mgr_;
    State state_ = State::INITIALIZING;
    QList<std::function<void ()>> when_online_;
};

}} // namepsaces

#endif // DSENGINE_H
