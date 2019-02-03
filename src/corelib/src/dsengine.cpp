
#include <assert.h>
#include <array>
#include <regex>

#include "ds/dsengine.h"
#include "ds/errors.h"
#include "ds/task.h"
#include "ds/dscert.h"
#include "ds/crypto.h"

#include <QString>
#include <QDebug>
#include <QDir>
#include <QUuid>
#include <QStandardPaths>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonDocument>
#include <QBuffer>

#include "logfault/logfault.h"

using namespace ds::crypto;
using namespace std;

namespace ds {
namespace core {

DsEngine *DsEngine::instance_;

DsEngine::DsEngine()
    : settings_{std::make_unique<QSettings>()}
{
    initialize();
}

DsEngine::DsEngine(std::unique_ptr<QSettings> settings)
    : settings_{move(settings)}
{
    initialize();
}

DsEngine::~DsEngine()
{
    assert(instance_ == this);
    instance_ = {};
}

DsEngine &DsEngine::instance()
{
    assert(instance_);
    return *instance_;
}

QSqlDatabase& DsEngine::getDb()
{
    assert(database_);
    return database_->getDb();
}

ProtocolManager &DsEngine::getProtocolMgr(ProtocolManager::Transport)
{
    assert(tor_mgr_);
    return *tor_mgr_;
}

const QByteArray& DsEngine::getName(const DsEngine::State state)
{
    static const array<QByteArray, 5> names = {{
        "INITIALIZING",
        "STARTING",
        "RUNNING",
        "CLOSING",
        "TERMINATED"
    }};

    return names.at(static_cast<size_t>(state));
}

bool DsEngine::isOnline() const
{
    if (tor_mgr_) {
        return tor_mgr_->isOnline();
    }

    return false;
}

QByteArray DsEngine::getIdentityHandle(const QByteArray &pubkey, const QByteArray &address)
{
    QVariantMap map;
    QVariantList list;
    list.append(address);
    map["pubkey"] = pubkey.toBase64();
    map.insert("address", list);

    return toJson(map);
}

void DsEngine::createIdentity(const IdentityReq& req)
{
    if (pending_identities_.contains(req.name)) {
        throw ExistsError(QStringLiteral("The name %1 is already in use").arg(req.name));
    }

    Identity id;
    id.name = req.name;
    id.avatar = req.avatar;
    id.notes = req.notes;
    id.uuid = req.uuid;

    pending_identities_.insert(req.uuid, id);

    auto name = req.name;
    auto uuid = id.uuid;
    whenOnline([this, name, uuid]() { tryMakeTransport(name, uuid); });

    // Create a cert for the user. This may take some time on slow devices.
    Task::schedule([this, name, uuid] {
        try {
            auto cert = DsCert::create();
            emit certCreated(uuid, cert);
        } catch (const std::exception& ex) {
            emit identityError({uuid, name, ex.what()});
        }
    });
}

void DsEngine::createContact(const ContactReq &req)
{
    static const regex valid_address{R"((^(onion):)?([a-z2-7]{16}|[a-z2-7]{56})\:(\d{1,5})$)"};

    Contact c;
    c.identity = req.identity;
    c.uuid = QUuid::createUuid().toByteArray();
    c.name = req.name;
    c.nickname = req.nickname;
    c.notes = req.notes;
    c.group = req.group;
    c.avatar = req.avatar;
    c.whoInitiated = req.whoInitiated;
    c.pubkey = req.pubkey;
    c.address = req.address;
    c.autoConnect = req.autoConnect;

    // Validate
    if (!regex_match(c.address.data(), valid_address)) {
        LFLOG_WARN << "New contact " << req.name
                   << " has an invalid address format : '"
                   << c.address
                   << "'";
        throw ParseError(QStringLiteral(
            "Invalid address format for contact %1").arg(req.name));
    }

    // Create a cert from the pubkey so we can hash it.
    auto cert = ds::crypto::DsCert::createFromPubkey(c.pubkey);
    c.hash = cert->getHash().toByteArray();

    // Emit
    LFLOG_DEBUG << "Contact " << c.name << " is ok.";
    LFLOG_DEBUG << "Hash is: " << c.hash.toBase64();
    emit contactCreated(c);
}

void DsEngine::createNewTransport(const QByteArray &name, const QUuid& uuid)
{
    tryMakeTransport(name, uuid);
}

void DsEngine::onCertCreated(const QUuid& uuid, const DsCert::ptr_t cert)
{
    if (!pending_identities_.contains(uuid)) {
        LFLOG_WARN << "Received a cert for a non-existing name: " << uuid.toString();
        return;
    }

    LFLOG_DEBUG << "Received cert for idenity " << uuid.toString();

    auto& id = pending_identities_[uuid];
    id.cert = cert->getCert();
    id.hash = cert->getHash().toByteArray();

    addIdentityIfReady(uuid);
}

void DsEngine::addIdentityIfReady(const QUuid &uuid)
{
    if (!pending_identities_.contains(uuid)) {
        const auto msg = QStringLiteral("No pending identity with that id");
        LFLOG_WARN << msg << ": " << uuid.toString();
        emit identityError({uuid, "", msg});
        return;
    }

    {
        const auto& pi = pending_identities_[uuid];
        if (pi.cert.isEmpty() || pi.address.isEmpty()) {
            LFLOG_DEBUG << "The identity " << uuid.toString() << " is still not ready";
            return;
        }
    }

    auto id = pending_identities_.take(uuid);
    LFLOG_DEBUG << "Identity " << id.uuid.toString() << " (" << id.name << ") is ready";
    emit identityCreated(id);
}

void DsEngine::whenOnline(std::function<void ()> fn)
{
    if (isOnline()) {
        fn();
    } else {
        when_online_.append(move(fn));
    }
}

void DsEngine::online()
{
    while(!when_online_.isEmpty()) {
        try {
            if (auto fn = when_online_.takeFirst()) {
                fn();
            }
        } catch (std::exception& ex) {
            LFLOG_WARN << "Failed to execute function when getting online: " << ex.what();
        }
    }
}

void DsEngine::onServiceFailed(const QUuid& serviceId, const QByteArray &reason)
{
    emit serviceFailed(serviceId, reason);
}

void DsEngine::onServiceStarted(const QUuid& serviceId, const bool newService)
{
    emit serviceStarted(serviceId, newService);
}

void DsEngine::onServiceStopped(const QUuid& serviceId)
{
    serviceStopped(serviceId);
}

void DsEngine::sendMessage(const Message& /*msg*/)
{
}

void DsEngine::onTransportHandleReady(const TransportHandle &th)
{
    // Relay
    emit transportHandleReady(th);

    if (!pending_identities_.contains(th.uuid)) {
        const auto msg = QStringLiteral("No pending identity with that name");
        LFLOG_WARN << msg << ": " << th.identityName;
        emit identityError({th.uuid, th.identityName, msg});
        return;
    }

    auto& pi = pending_identities_[th.uuid];
    if (pi.address.isEmpty()) {
        pi.address = th.handle;
        pi.addressData = toJson(th.data);
    } else {
        LFLOG_DEBUG << "The identity " << th.identityName << " already have a transport";
        return;
    }

    emit retryIdentityReady(th.uuid);
}

void DsEngine::onTransportHandleError(const TransportHandleError &the)
{
    emit transportHandleError(the);

    auto name = the.identityName;
    auto uuid = the.uuid;
    LFLOG_DEBUG << "Transport-handle creaton failed: " << name
             << ". I Will try again.";
    whenOnline([this, name, uuid]() { tryMakeTransport(name, uuid); });
}

void DsEngine::close()
{
    setState(State::CLOSING);
    if (tor_mgr_) {
        tor_mgr_->stop();
    }
}

void DsEngine::start()
{
    setState(State::STARTING);
    tor_mgr_ = ProtocolManager::create(*settings_, ProtocolManager::Transport::TOR);

    connect(tor_mgr_.get(), &ProtocolManager::stateChanged, this, &DsEngine::onStateChanged);

    connect(tor_mgr_.get(), &ProtocolManager::transportHandleReady,
            this, &DsEngine::onTransportHandleReady);

    connect(tor_mgr_.get(), &ProtocolManager::transportHandleError,
            this, &DsEngine::onTransportHandleError,
            Qt::QueuedConnection);

    connect(this, &DsEngine::retryIdentityReady,
            this, &DsEngine::addIdentityIfReady,
            Qt::QueuedConnection);

    connect(this, &DsEngine::ready,
            this, &DsEngine::online,
            Qt::QueuedConnection);

    connect(tor_mgr_.get(),
            &ds::core::ProtocolManager::serviceStarted,
            this, &DsEngine::onServiceStarted);

    connect(tor_mgr_.get(),
            &ds::core::ProtocolManager::serviceStopped,
            this, &DsEngine::onServiceStopped);

    connect(tor_mgr_.get(),
            &ds::core::ProtocolManager::serviceFailed,
            this, &DsEngine::onServiceFailed);

    connect(tor_mgr_.get(),
            &ds::core::ProtocolManager::connectedTo,
            this, [this](const QUuid& uuid) {
        emit connectedTo(uuid);
    });

    connect(tor_mgr_.get(),
            &ds::core::ProtocolManager::disconnectedFrom,
            this, [this]( const QUuid& uuid) {
        emit disconnectedFrom(uuid);
    });

    connect(tor_mgr_.get(),
            &ds::core::ProtocolManager::connectionFailed,
            this, [this](const QUuid& uuid,
            const QAbstractSocket::SocketError& socketError) {
        emit connectionFailed(uuid, socketError);
    });

    connect(tor_mgr_.get(),
            &ds::core::ProtocolManager::incomingPeer,
            this, [this](const QUuid& service, const QUuid& connectionId, const QByteArray& handle) {
        emit incomingPeer(service, connectionId, handle);
    });

    tor_mgr_->start();
}

void DsEngine::onStateChanged(const ProtocolManager::State old, const ProtocolManager::State current)
{
    switch (current) {
    case ProtocolManager::State::OFFLINE:
        if (state_ == State::CLOSING) {
            setState(State::TERMINATED);
        } else {
            setState(State::INITIALIZING);
        }
        break;
    case ProtocolManager::State::CONNECTING:
        setState(State::STARTING);
        break;
    case ProtocolManager::State::CONNECTED:
        setState(State::RUNNING);
        break;
    case ProtocolManager::State::ONLINE:
        break;
    case ProtocolManager::State::SHUTTINGDOWN:
        break;
    }

    LFLOG_DEBUG << "DsEngine: Emitting online state changed to " << current;
    emit onlineStateChanged(old, current);
}


void DsEngine::initialize()
{
    assert(!instance_);
    instance_ = this;
    LFLOG_DEBUG << "Settings are in " << settings_->fileName();

    static bool initialized = false;
    if (!initialized) {
        qRegisterMetaType<ds::core::Identity>("ds::core::Identity");
        qRegisterMetaType<ds::core::Identity>("Identity");
        qRegisterMetaType<ds::core::Contact>("ds::core::Contact");
        qRegisterMetaType<ds::core::Contact>("Contact");
        qRegisterMetaType<ds::core::TransportHandleError>("TransportHandleError");
    }

    auto data_path = QStandardPaths::writableLocation(
                QStandardPaths::AppDataLocation);
    if (!QDir(data_path).exists()) {
        LFLOG_DEBUG << "Creating path: " << data_path;
        QDir().mkpath(data_path);
    }

    switch(auto version = settings_->value("version").toInt()) {
        case 0: { // First use
            qInfo() << "First use - initializing settings";
            settings_->setValue("version", 1);
        } break;
    default:
        LFLOG_DEBUG << "Settings are OK at version " << version;
    }

    if (settings_->value("dbpath", "").toString().isEmpty()) {
        QString dbpath = data_path;
#ifdef QT_DEBUG
        dbpath += "/darkspeak-debug.db";
#else
        dbpath += "/darkspeak.db";
#endif
        settings_->setValue("dbpath", dbpath);
    }

    if (settings_->value("log-path", "").toString().isEmpty()) {
        QString logpath = data_path;
#ifdef QT_DEBUG
        logpath += "/darkspeak-debug.log";
#else
        logpath += "/darkspeak.log";
#endif
        settings_->setValue("log-path", logpath);
    }

    database_ = std::make_unique<Database>(*settings_);

    connect(this, &DsEngine::certCreated, this, &DsEngine::onCertCreated);
}

void DsEngine::setState(DsEngine::State state)
{
    if (state_ != state) {
        auto old = state_;
        state_ = state;
        LFLOG_DEBUG << "DsEngine: Changing state from "
                 << getName(old) << " to " << getName(state);
        emit stateChanged(old, state);

        switch(state) {
        case State::INITIALIZING:
            emit notReady();
            break;
        case State::RUNNING:
            emit ready();
            break;
        case State::CLOSING:
            emit closing();
            break;
        default:
            ; // Do nothing
        }
    }
}

void DsEngine::tryMakeTransport(const QString &name, const QUuid& uuid)
{
    TransportHandleReq req{name, uuid};

    try {
        tor_mgr_->createTransportHandle(req);
    } catch (const std::exception& ex) {
        LFLOG_DEBUG << "Failed to create transport for " << name
                 << " (will try again later): "
                 << ex.what();

        if (isOnline()) {
            QTimer::singleShot(1000, this, [this, name, uuid]() {
                whenOnline([this, name, uuid]() { tryMakeTransport(name, uuid); });
            });

            // TODO: Fail after n retries
        } else {
            whenOnline([this, name, uuid]() { tryMakeTransport(name, uuid); });
        }
    }
}

QByteArray DsEngine::toJson(const QVariantMap &data)
{
    auto json = QJsonDocument::fromVariant(data);
    return json.toJson(QJsonDocument::Compact);
}

QVariantMap DsEngine::fromJson(const QByteArray &json)
{
    auto doc = QJsonDocument::fromJson(json);
    if (doc.isNull()) {
        throw ParseError("Failed to parse json data");
    }

    return doc.toVariant().value<QVariantMap>();
}

QByteArray DsEngine::imageToBytes(const QImage &img)
{
    QByteArray ba;
    QBuffer buffer(&ba);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    return ba;
}


}} // namespaces
