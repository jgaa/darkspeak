
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
    : settings_{std::make_unique<QSettings>(
                    QStringLiteral("Jgaas Internet"),
                    QStringLiteral("DarkSpeak"))}
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

    pending_identities_.insert(req.name, id);

    auto name = req.name;
    whenOnline([this, name]() { tryMakeTransport(name); });

    // Create a RSA cert for the user. This may take some time on slow devices.
    Task::schedule([this, name] {
        // TODO: Take cert type from settings
        try {
            auto cert = DsCert::create();
            emit certCreated(name, cert);
        } catch (const std::exception& ex) {
            emit identityError({name, ex.what()});
        }
    });
}

void DsEngine::createContact(const ContactReq &req)
{
    static const regex valid_address{R"(^(onion):([a-z2-7]{16}|[a-z2-7]{56}):(\d{1,5})$)"};

    // Parse handle
    auto map = fromJson(req.contactHandle);

    Contact c;
    c.identity = req.identity;
    c.name = req.name;
    c.nickname = req.nickname;
    c.notes = req.notes;
    c.group = req.group;
    c.avatar = req.avatar;
    c.whoInitiated = req.whoInitiated;

    c.pubkey = QByteArray::fromBase64(map.value("pubkey").toByteArray());

    auto addresses = map.value("address").toList();
    if (addresses.size() != 1) {
        throw ParseError(QStringLiteral(
            "Unrecognized address format in contact %1").arg(req.name));
    }
    c.address = addresses.front().toByteArray();

    // Validate
    if (!regex_match(c.address.data(), valid_address)) {
        qWarning() << "New contact " << req.name
                   << " has an invalid address format : '"
                   << c.address
                   << "'";
        throw ParseError(QStringLiteral(
            "Invalid address format for contact %1").arg(req.name));
    }

    // Create a cert from the pubkey so we can hash it.
    auto cert = ds::crypto::DsCert::createFromPubkey(c.pubkey);
    c.hash = cert->getHash();

    // Emit
    LFLOG_DEBUG << "Contact " << c.name << " is ok.";
    LFLOG_DEBUG << "Hash is: " << c.hash.toBase64();
    emit contactCreated(c);
}

void DsEngine::onCertCreated(const QString &name, const DsCert::ptr_t cert)
{
    if (!pending_identities_.contains(name)) {
        qWarning() << "Received a cert for a non-existing name: " << name;
        return;
    }

    LFLOG_DEBUG << "Received cert for idenity " << name;

    auto& id = pending_identities_[name];
    id.cert = cert->getCert();
    id.hash = cert->getHash();

    addIdentityIfReady(name);
}

void DsEngine::addIdentityIfReady(const QString &name)
{
    if (!pending_identities_.contains(name)) {
        const auto msg = QStringLiteral("No pending identity with that name");
        qWarning() << msg << ": " << name;
        emit identityError({name, msg});
        return;
    }

    {
        const auto& pi = pending_identities_[name];
        if (pi.cert.isEmpty() || pi.address.isEmpty()) {
            LFLOG_DEBUG << "The identity " << name << " is still not ready";
            return;
        }
    }

    auto id = pending_identities_.take(name);
    LFLOG_DEBUG << "Identity " << id.name << " is ready";
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
            qWarning() << "Failed to execute function when getting online: " << ex.what();
        }
    }
}

void DsEngine::sendMessage(const Message& msg)
{
}

void DsEngine::onTransportHandleReady(const TransportHandle &th)
{
    const auto name = th.identityName;
    if (!pending_identities_.contains(name)) {
        const auto msg = QStringLiteral("No pending identity with that name");
        qWarning() << msg << ": " << name;
        emit identityError({name, msg});
        return;
    }

    auto& pi = pending_identities_[name];
    if (pi.address.isEmpty()) {
        pi.address = th.handle;
        pi.addressData = toJson(th.data);
    } else {
        LFLOG_DEBUG << "The identity " << name << " already have a transport";
        return;
    }

    emit retryIdentityReady(name);
}

void DsEngine::onTransportHandleError(const TransportHandleError &th)
{
    auto name = th.identityName;
    LFLOG_DEBUG << "Transport-handle creaton failed: " << name
             << ". I Will try again.";
    whenOnline([this, name]() { tryMakeTransport(name); });
}

void DsEngine::close()
{
    setState(State::CLOSING);
    tor_mgr_->stop();
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

    tor_mgr_->start();
}

void DsEngine::onStateChanged(const ProtocolManager::State /*old*/, const ProtocolManager::State current)
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

void DsEngine::tryMakeTransport(const QString &name)
{
    TransportHandleReq req{name};

    try {
        tor_mgr_->createTransportHandle(req);
    } catch (const std::exception& ex) {
        LFLOG_DEBUG << "Failed to create transport for " << name
                 << " (will try again later): "
                 << ex.what();

        if (isOnline()) {
            QTimer::singleShot(1000, this, [this, name]() {
                whenOnline([this, name]() { tryMakeTransport(name); });
            });

            // TODO: Fail after n retries
        } else {
            whenOnline([this, name]() { tryMakeTransport(name); });
        }
    }
}

QByteArray DsEngine::toJson(const QVariantMap &data)
{
    auto json = QJsonDocument::fromVariant(data);
    return json.toJson();
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
