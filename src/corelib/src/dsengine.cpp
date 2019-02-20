
#include <assert.h>
#include <array>
#include <regex>

#include "ds/dsengine.h"
#include "ds/errors.h"
#include "ds/task.h"
#include "ds/dscert.h"
#include "ds/crypto.h"
#include "ds/identity.h"
#include "ds/base32.h"

#include <QString>
#include <QDebug>
#include <QDir>
#include <QUuid>
#include <QStandardPaths>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QBuffer>
#include <QtEndian>

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

IdentityManager *DsEngine::getIdentityManager()
{
    return identityManager_;
}

ContactManager *DsEngine::getContactManager()
{
    return contactManager_;
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


void DsEngine::createNewTransport(const QByteArray &name, const QUuid& uuid)
{
    tryMakeTransport(name, uuid);
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

    identityManager_->onOnline();
}

void DsEngine::onServiceFailed(const QUuid& serviceId, const QByteArray &reason)
{
    emit serviceFailed(serviceId, reason);
}

void DsEngine::onServiceStarted(const QUuid& serviceId, const bool newService)
{
    emit serviceStarted(serviceId, newService);

    if (!newService) {
        if (auto identity = identityManager_->identityFromUuid(serviceId)) {
            identity->setOnline(true);
        }
    }
}

void DsEngine::onServiceStopped(const QUuid& serviceId)
{
    serviceStopped(serviceId);

    if (auto identity = identityManager_->identityFromUuid(serviceId)) {
        identity->setOnline(false);
    }
}

void DsEngine::onReceivedData(const QUuid &service,
                              const QUuid &connectionId,
                              const quint32 channel,
                              const quint64 id,
                              const QByteArray &data)
{
    emit receivedData(service, connectionId, channel, id, data);

    if (channel == 0) {
        // Control channel. Data is supposed to be Json.
        QJsonDocument json = QJsonDocument::fromJson(data);
        if (json.isNull()) {
            LFLOG_ERROR << "Incoming data on " << connectionId.toString()
                        << " with id=" << id
                        << " is supposed to be in Json format, but it is not.";
            throw runtime_error("Not Json");
        }

        const auto type = json.object().value("type");

        if (type == "AddMe") {
            PeerAddmeReq req{service, connectionId, id,
                        json.object().value("nick").toString(),
                        json.object().value("message").toString(),
                        json.object().value("address").toString().toUtf8(),
                        tor_mgr_->getPeerHandle(service, connectionId)};

            emit receivedAddMe(req);
        } else if (type == "Ack") {
            PeerAck ack{service, connectionId, id,
                        json.object().value("what").toString().toUtf8(),
                        json.object().value("status").toString().toUtf8()};

            emit receivedAck(ack);
        } else {
            throw runtime_error("Unrecognized type");
        }
    }
}

void DsEngine::onTransportHandleReady(const TransportHandle &th)
{
    if (auto identity = identityManager_->identityFromUuid(th.uuid)) {
        LFLOG_NOTICE << "Assigning new handle " << th.handle
                     << " to identity " << identity->getName();
        identity->setAddress(th.handle);
        identity->setAddressData(toJson(th.data));
    }
}

void DsEngine::onTransportHandleError(const TransportHandleError &the)
{
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
            this, [this](const QUuid& uuid, const ProtocolManager::Direction direction) {
        emit connectedTo(uuid, direction);
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
            this, [this](const QUuid& service, const QUuid& connectionId,
            const QByteArray& handle) {
        emit incomingPeer(service, connectionId, handle);
    });

    connect(tor_mgr_.get(), &ds::core::ProtocolManager::receivedData,
            this, &DsEngine::onReceivedData);

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
        qRegisterMetaType<ds::core::IdentityData>("ds::core::IdentityData");
        qRegisterMetaType<ds::core::IdentityData>("IdentityData");
        qRegisterMetaType<ds::core::IdentityError>("ds::core::IdentityError");
        qRegisterMetaType<ds::core::IdentityError>("IdentityError");
        qRegisterMetaType<ds::core::TransportHandleError>("TransportHandleError");
        qRegisterMetaType<ds::core::IdentityReq>("IdentityReq");
        qRegisterMetaType<ds::core::QmlIdentityReq *>("const QmlIdentityReq *");
        qRegisterMetaType<ds::core::Identity *>("Identity *");
        qRegisterMetaType<ds::core::Contact *>("Contact *");
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

    identityManager_ = new IdentityManager(*this);
    contactManager_ = new ContactManager(*this);
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

//Contact DsEngine::prepareContact(const ContactReq &req)
//{
//    static const regex valid_address{R"((^(onion):)?([a-z2-7]{16}|[a-z2-7]{56})\:(\d{3,5})$)"};

//    Contact c;
//    c.identity = req.identity;
//    c.uuid = QUuid::createUuid().toByteArray();
//    c.name = req.name;
//    c.nickname = req.nickname;
//    c.notes = req.notes;
//    c.group = req.group;
//    c.avatar = req.avatar;
//    c.whoInitiated = req.whoInitiated;
//    c.pubkey = req.pubkey;
//    c.address = req.address;
//    c.autoConnect = req.autoConnect;
//    c.addmeMessage = req.addmeMessage;

//    // Validate
//    if (!regex_match(c.address.data(), valid_address)) {
//        LFLOG_WARN << "New contact " << req.name
//                   << " has an invalid address format : '"
//                   << c.address
//                   << "'";
//        throw ParseError(QStringLiteral(
//            "Invalid address format for contact %1").arg(req.name));
//    }

//    // Create a cert from the pubkey so we can hash it.
//    auto cert = ds::crypto::DsCert::createFromPubkey(c.pubkey);
//    c.hash = cert->getHash().toByteArray();

//    // Emit
//    LFLOG_DEBUG << "Contact " << c.name << " is ok.";
//    LFLOG_DEBUG << "Hash is: " << c.hash.toBase64();

//    return c;
//}

QByteArray DsEngine::getIdentityAsBase58(const DsCert::ptr_t &cert, const QByteArray &address)
{
    // Format:
    //  1 byte version (1)
    //  32 byte pubkey
    //  10 bytes onion address
    //  2 bytes port

    // B58 tag: {11, 176}

    QByteArray bytes;

    bytes.append('\1'); // Version
    bytes += cert->getSigningPubKey().toByteArray();

    // Parse the address
    std::string onion, port;
    static const regex legacy_pattern{R"(^(onion:)?([a-z2-7]{16})\:(\d{3,5})$)"};

    std::smatch m;
    std::string str = address.toStdString();
    if (std::regex_match(str, m, legacy_pattern)) {
        onion = m[2].str();
        port = m[3].str();
    } else {
        LFLOG_ERROR << "getIdentityAsBase58: Invalid address: " << address;
        return {};
    }

    if (onion.size() == 16) {
        // Legacy onion address
        bytes += crypto::onion16decode({onion.c_str()});
    } else {
        // TODO: Implement
        LFLOG_ERROR << "getIdentityAsBase58 NOT IMPLEMENTED for modern Tor addresses!";
        return "Sorry, Not implemented";
    }

    // Add the port
    union {
        char bytes[2];
        uint16_t port;
    } port_u;

    port_u.port = qToBigEndian(static_cast<uint16_t>(atoi(port.c_str())));
    bytes += port_u.bytes[0];
    bytes += port_u.bytes[1];

    return crypto::b58check_enc<QByteArray>(bytes, {11, 176});
}


}} // namespaces
