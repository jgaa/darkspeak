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
#include "ds/identitymanager.h"
#include "ds/contactmanager.h"
#include "ds/conversation.h"
#include "ds/conversationmanager.h"

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
    IdentityManager *getIdentityManager();
    ContactManager *getContactManager();
    ConversationManager *getConversationManager();

    QSettings& settings() noexcept { return *settings_; }
    ProtocolManager& getProtocolMgr(ProtocolManager::Transport transport);
    static const QByteArray& getName(const State state);
    bool isOnline() const;
    static QByteArray getIdentityHandle(const QByteArray& pubkey, const QByteArray& address);
    static QByteArray toJson(const QVariantMap& data);
    static QVariantMap fromJson(const QByteArray& json);
    static QByteArray imageToBytes(const QImage& img);
    static QByteArray getIdentityAsBase58(const crypto::DsCert::ptr_t& cert,
                                          const QByteArray& address);
    void whenOnline(std::function<void ()> fn);

public slots:
    void createNewTransport(const QByteArray& name, const QUuid& uuid);
    void close();
    void start();

private slots:
    void onStateChanged(const ProtocolManager::State old, const ProtocolManager::State current);
    void onTransportHandleReady(const TransportHandle& th);
    void onTransportHandleError(const TransportHandleError& th);
    void online();
    void onServiceFailed(const QUuid& id, const QByteArray& reason);
    void onServiceStarted(const QUuid& id, const bool newService);
    void onServiceStopped(const QUuid& id);
//    void onReceivedData(const QUuid& service, const QUuid& connectionId, const quint32 channel,
//                        const quint64 id, const QByteArray& data);

signals:
    void identityCreated(Identity *);
    void identityError(const IdentityError&);
    void contactCreated(const Contact& contact);
    void ready();
    void notReady(); // If the transport goes offline
    void closing();
    void stateChanged(const State from, const State to);
    void onlineStateChanged(const ProtocolManager::State old, const ProtocolManager::State current);
    void serviceFailed(const QUuid& uuid, const QByteArray& reason);
    void serviceStarted(const QUuid& uuid, const bool newService);
    void serviceStopped(const QUuid& uuid);
//    void connectedTo(const QUuid& identity, const QUuid& uuid, const ProtocolManager::Direction direction);
//    void disconnectedFrom(const QUuid& identity, const QUuid& uuid);
//    void connectionFailed(const QUuid& uuid,
//                          const QAbstractSocket::SocketError& socketError);
    void incomingPeer(const std::shared_ptr<PeerConnection>& peer);
//    void receivedData(const QUuid& service, const QUuid& connectionId, const quint32 channel,
//                      const quint64 id, const QByteArray& data);
//    void receivedAddMe(const PeerAddmeReq& req);
//    void receivedAck(const PeerAck& req);

protected:
    void initialize();
    void setState(State state);
    void tryMakeTransport(const QString& name, const QUuid& uuid);

    std::unique_ptr<QSettings> settings_;
    std::unique_ptr<Database> database_;
    static DsEngine *instance_;
    ProtocolManager::ptr_t tor_mgr_;
    State state_ = State::INITIALIZING;
    QList<std::function<void ()>> when_online_;
    IdentityManager *identityManager_ = nullptr;
    ContactManager *contactManager_ = nullptr;
    ConversationManager *conversationManager_ = nullptr;
};

}} // namepsaces



#endif // DSENGINE_H
