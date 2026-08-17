
#include <cassert>
#include <array>
#include <regex>
#include <iostream>

#include "ds/dsengine.h"
#include "ds/errors.h"
#include "ds/task.h"
#include "ds/dscert.h"
#include "ds/crypto.h"
#include "ds/identity.h"
#include "ds/base32.h"
#include "ds/database.h"
#include "ds/logutil.h"
#include "ds/bytes.h"
#include "ds/memoryview.h"
#include "ds/resourcelimits.h"

#include <QString>
#include <QDebug>
#include <QDir>
#include <QFile>
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
using namespace logfault;

namespace ds {
namespace core {

DsEngine *DsEngine::instance_;

DsEngine::DsEngine()
    : settings_{std::make_unique<QSettings>()}
{
    try {
        initialize();
    } catch (...) {
        // initialize() publishes the singleton before constructing dependent
        // services.  Do not leave a dangling singleton if a secure service
        // (such as the OS vault) prevents construction from completing.
        instance_ = {};
        throw;
    }
}

DsEngine::DsEngine(std::unique_ptr<QSettings> settings)
    : settings_{move(settings)}
{
    try {
        initialize();
    } catch (...) {
        instance_ = {};
        throw;
    }
}

DsEngine::~DsEngine()
{
    if (secretStore_) {
        secretStore_->lock();
    }
    assert(instance_ == this);
    instance_ = {};
}

DsEngine &DsEngine::instance()
{
    assert(instance_);
    return *instance_;
}

DsEngine::State DsEngine::getState() const
{
    return state_;
}

QSqlDatabase& DsEngine::getDb()
{
    assert(database_);
    return database_->getDb();
}

SecretStore& DsEngine::secretStore()
{
    assert(secretStore_);
    return *secretStore_;
}

IdentityManager *DsEngine::getIdentityManager()
{
    return identityManager_;
}

ContactManager *DsEngine::getContactManager()
{
    return contactManager_;
}

ConversationManager *DsEngine::getConversationManager()
{
    return conversationManager_;
}

MessageManager *DsEngine::getMessageManager()
{
    return messageManager_;
}

FileManager *DsEngine::getFileManager()
{
    return fileManager_;
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

QString DsEngine::getTorVersion() const
{
    return tor_mgr_ ? tor_mgr_->getTorVersion() : QString{};
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

void DsEngine::whenOnline(const std::function<void ()>& fn)
{
    if (isOnline()) {
        fn();
    } else {
        when_online_.append(fn);
    }
}

void DsEngine::reportStatus(const QString& message, const StatusLevel level)
{
    if (!message.isEmpty()) {
        emit statusMessage(message, level);
    }
}

QDateTime DsEngine::getSafeNow() noexcept
{
    return getSafeTime(QDateTime::currentDateTime());
}

QDateTime DsEngine::getSafeTime(const QDateTime &when) noexcept
{
    auto secs = when.toSecsSinceEpoch();
    secs /= 60;
    secs *= 60;
    return QDateTime::fromSecsSinceEpoch(secs, QTimeZone::UTC);
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
    reportStatus(QStringLiteral("%1: %2").arg(serviceId.toString(),
                                               QString::fromUtf8(reason)),
                 StatusLevel::ERROR);
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

void DsEngine::onTransportHandleReady(const TransportHandle &th)
{
    if (auto identity = identityManager_->identityFromUuid(th.uuid)) {
        LFLOG_NOTICE << "Assigning new handle " << th.handle
                     << " to identity " << identity->getName();
        identity->setAddress(th.handle);
        auto json = toJson(th.data);
        if (const auto key = th.data.value("key").toByteArray(); !key.isEmpty()) {
            secretStore().storeTorServiceKey(th.uuid, key);
            auto publicData = th.data;
            publicData.remove("key");
            json = toJson(publicData);
        }
        identity->setAddressData(json);
        if (getState() == RUNNING && isOnline() && identity->isAutoConnect()) {
            identity->startService();
        }
    }
}

void DsEngine::onTransportHandleError(const TransportHandleError &the)
{
    if (auto identity = getIdentityManager()->identityFromUuid(the.uuid)) {
        if (!identity->getAddress().isEmpty()) {
            LFLOG_WARN << "Failed to start " << identity->getAddress()
                       << " for identity " << identity->getName()
                       << ": " << the.explanation;

            if (identity->isOnline()) {
                identity->stopService();
            }

            return; // Don't create a new service ...
        }
    }

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
        identityManager_->disconnectAll();
        tor_mgr_->stop();

        try {
            tor_mgr_.reset();
        } catch (const std::exception& ex) {
            LFLOG_ERROR << "Error when shutting down Tor Manager: " << ex.what();
        }
    }
}

void DsEngine::start()
{
    setState(State::STARTING);
    tor_mgr_ = ProtocolManager::create(*settings_, ProtocolManager::Transport::TOR);

    connect(tor_mgr_.get(), &ProtocolManager::stateChanged, this, &DsEngine::onStateChanged);
    connect(tor_mgr_.get(), &ProtocolManager::torVersionChanged,
            this, &DsEngine::torVersionChanged);

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
            &ds::core::ProtocolManager::incomingPeer,
            this, [this](const std::shared_ptr<PeerConnection>& peer) {
        emit incomingPeer(peer);

        identityManager_->onIncomingPeer(peer);
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
    ensureLogSettings(*settings_);
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
        qRegisterMetaType<ds::core::Conversation *>("Conversation *");
        qRegisterMetaType<ds::core::Conversation *>("core::Conversation *");
        qRegisterMetaType<ds::core::Conversation::ptr_t>("ds::core::Conversation::ptr_t");
        qRegisterMetaType<ds::core::PeerConnection::ptr_t>("ds::core::PeerConnection::ptr_t");
        qRegisterMetaType<ds::core::Conversation *>("Message *");
        qRegisterMetaType<ds::core::Conversation *>("MessageData");
        qRegisterMetaType<ds::core::Conversation *>("File *");
        qRegisterMetaType<ds::core::Conversation *>("FileData");
    }

    QString data_path;
    if (qEnvironmentVariableIsSet("DARKSPEAK_PROFILE_ROOT")) {
        data_path = QDir(qEnvironmentVariable("DARKSPEAK_PROFILE_ROOT"))
                .filePath("data");
    } else {
        data_path = QStandardPaths::writableLocation(
                    QStandardPaths::AppDataLocation);
    }
    if (!QDir(data_path).exists()) {
        LFLOG_DEBUG << "Creating path: " << data_path;
        QDir().mkpath(data_path);
    }
    QFile::setPermissions(data_path,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner
                          | QFileDevice::ExeOwner);

    switch(auto version = settings_->value("version").toInt()) {
        case 0: { // First use
            qInfo() << "First use - initializing settings";
            settings_->setValue("version", 1);
        } break;
    default:
        LFLOG_DEBUG << "Settings are OK at version " << version;
    }

    if (!settings_->contains("appAutoConnect")) {
        settings_->setValue("appAutoConnect", true);
    }

    if (!settings_->contains("useDarkSpeakFolder")) {
        settings_->setValue("useDarkSpeakFolder", false);
    }

    if (!settings_->contains("torMode")) {
        settings_->setValue("torMode", 1); // Embedded Tor
    }
    if (!settings_->contains("torCmd")) {
        settings_->setValue("torCmd", "tor");
    }

    if (!settings_->contains("autoAcceptIncomingFiles")) {
        settings_->setValue("autoAcceptIncomingFiles", false);
    }
    if (!settings_->contains("includeConversationNameInDownloadPath")) {
        settings_->setValue("includeConversationNameInDownloadPath", false);
    }
    const auto limits = core::ResourceLimits::fromSettings(*settings_);
    settings_->setValue("resourceUnapprovedConnections", limits.unapprovedConnections);
    settings_->setValue("resourceOutstandingRequests", limits.outstandingRequests);
    settings_->setValue("resourcePendingConnectionRequests", limits.pendingConnectionRequests);
    settings_->setValue("resourceActiveFileTransfers", limits.activeFileTransfers);
    settings_->setValue("resourceQueuedFileOffers", limits.queuedFileOffers);
    settings_->setValue("resourceRetainedInterruptedTransfers", limits.retainedInterruptedTransfers);
    settings_->setValue("resourceAcceptedFileBytes",
                        static_cast<qulonglong>(limits.acceptedFileBytes));
    settings_->setValue("resourceUnfinishedIncomingBytes",
                        static_cast<qulonglong>(limits.unfinishedIncomingBytes));
    settings_->setValue("resourceIncomingDiskBytes",
                        static_cast<qulonglong>(limits.incomingDiskBytes));
    settings_->setValue("resourceMinimumFreeSpacePercent",
                        limits.minimumFreeSpacePercent);
    settings_->setValue("resourceSocketOutputBytes", limits.socketOutputBytes);

    if (settings_->value("dbpath", "").toString().isEmpty()) {
        QString dbpath = data_path;
#ifdef QT_DEBUG
        dbpath += "/darkspeak-debug.db";
#else
        dbpath += "/darkspeak.db";
#endif
        settings_->setValue("dbpath", dbpath);
    }

    if (settings_->value("downloadLocation", "").toString().isEmpty()) {
        QString downloadPath;
        if (qEnvironmentVariableIsSet("DARKSPEAK_PROFILE_ROOT")) {
            downloadPath = QDir(qEnvironmentVariable("DARKSPEAK_PROFILE_ROOT"))
                    .filePath("data/downloads");
        } else {
            downloadPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
                    + "/darkspeak";
        }
        QDir path = downloadPath;
        settings_->setValue("downloadLocation", path.path());
        if (!path.exists()) {
            LFLOG_NOTICE << "Creating download directory \"" << path.path() << "\".";
            path.mkpath(path.path());
        }
        QFile::setPermissions(path.path(),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner
                              | QFileDevice::ExeOwner);
    }

    if (settings_->value("logPath", "").toString().isEmpty()) {
        QString logpath = data_path;
#ifdef QT_DEBUG
        logpath += "/darkspeak-debug.log";
#else
        logpath += "/darkspeak.log";
#endif
        settings_->setValue("logPath", logpath);
    }

    secretStore_ = std::make_unique<SecretStore>(settings_->value("dbpath").toString());
    database_ = std::make_unique<Database>(*settings_);

    identityManager_ = new IdentityManager(*this);
    contactManager_ = new ContactManager(*this);
    conversationManager_ = new ConversationManager(*this);
    messageManager_ = new MessageManager(*this);
    fileManager_ = new FileManager(*this, *settings_);
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

QByteArray DsEngine::getIdentityAsBase58(const DsCert::ptr_t &cert, const QByteArray &address)
{
    // Format:
    //  1 byte version (2)
    //  32 byte pubkey
    //  35 bytes Tor v3 address payload
    //  2 bytes port

    // B58 tag: {11, 176}

    QByteArray bytes;

    bytes.append('\2'); // Version
    bytes += cert->getSigningPubKey().toByteArray();

    // Parse the address
    std::string onion, port;
    static const regex v3_pattern{R"(^(onion:)?([a-z2-7]{56})(\.onion)?\:(\d{1,5})$)"};

    std::smatch m;
    std::string str = address.toStdString();
    if (std::regex_match(str, m, v3_pattern)) {
        onion = m[2].str();
        port = m[4].str();
    } else {
        LFLOG_ERROR << "getIdentityAsBase58: Invalid address: " << address;
        return {};
    }

    if (onion.size() != 56) {
        LFLOG_ERROR << "getIdentityAsBase58: legacy Tor address rejected";
        return {};
    }
    bytes += crypto::onionv3decode(QByteArray::fromStdString(onion));

    appendValueToBytes(qToBigEndian(static_cast<uint16_t>(atoi(port.c_str()))), bytes);

    return crypto::b58check_enc<QByteArray>(bytes, {11, 176});
}


}} // namespaces
