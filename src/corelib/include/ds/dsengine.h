#ifndef DSENGINE_H
#define DSENGINE_H

#include <memory>

#include <QObject>
#include <QSettings>

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
    void sendMessage(const Message& message);
    void close();
    void start();

private slots:
    void onStateChanged(const ProtocolManager::State old, const ProtocolManager::State current);
    void onCertCreated(const QString& name, const ds::crypto::DsCert::ptr_t cert);
    void addIdentityIfReady(const QString& name);
    void whenOnline(std::function<void ()> fn);
    void onTransportHandleReady(const TransportHandle& th);
    void onTransportHandleError(const TransportHandleError& th);
    void online();

signals:
    void identityCreated(const Identity&);
    void identityError(const IdentityError&);
    void contactCreated(const Contact& contact);
    void ready();
    void notReady(); // If the transport goes offline
    void closing();
    void stateChanged(const State from, const State to);
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
