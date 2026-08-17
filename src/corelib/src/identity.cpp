#include <random>

#include "ds/dsengine.h"
#include "ds/identity.h"
#include "ds/avatarutil.h"
#include "ds/errors.h"
#include "ds/update_helper.h"
#include "ds/bio.h"
#include "ds/dscert.h"
#include "ds/base58.h"
#include "ds/base32.h"
#include "ds/bytes.h"

#include "logfault/logfault.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QCryptographicHash>
#include <QTimer>
#include <QRegularExpression>
#include <QFile>
#include <QStringList>
#include <QtEndian>

namespace ds {
namespace core {

using namespace std;

Identity::Identity(QObject& parent,
             const int dbId, // -1 if the identity is new
             const bool online,
             QDateTime created,
             IdentityData data)
    : QObject{&parent}
    , id_{dbId}, online_{online}, data_{std::move(data)}, created_{move(created)}
{
    data_.bio = bio::sanitize(data_.bio);
    const auto prefix = QStringLiteral("identity/%1/").arg(
        data_.uuid.toString(QUuid::WithoutBraces));
    auto& settings = DsEngine::instance().settings();
    settings.setValue(prefix + QStringLiteral("allowUtf8"), data_.allowUtf8);
    settings.setValue(prefix + QStringLiteral("allowFiles"), data_.allowFiles);
    settings.setValue(prefix + QStringLiteral("allowMimeTypes"), data_.allowMimeTypes);
    settings.setValue(prefix + QStringLiteral("allowReactions"), data_.allowReactions);
    settings.setValue(prefix + QStringLiteral("publicBio"), data_.publicBio);
    settings.setValue(prefix + QStringLiteral("bio"), data_.bio);
    settings.setValue(prefix + QStringLiteral("publicAvatar"), data_.publicAvatar);

    connect(this, &Identity::processOnlineLater,
            this, &Identity::onProcessOnlineLater,
            Qt::QueuedConnection);
}

void Identity::addContact(const QVariantMap &args)
{
    LFLOG_DEBUG << "Adding contact: " << args.value("name").toString();

    auto data = make_unique<ContactData>();

    data->identity = args.value("identity").toInt();
    data->name = args.value("name").toString();

    auto identityId = args.value("identityId").toString().trimmed();
    const auto separator = identityId.indexOf(':');
    if (separator > 0)
        identityId = identityId.mid(separator + 1);

    constexpr auto payloadLength = 1 + 32 + 35 + 2;
    QByteArray identityData;
    try {
        identityData = crypto::b58tobin_check<QByteArray>(
            identityId.toStdString(), payloadLength, {11, 176});
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Invalid identity ID: " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Invalid identity ID: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
        return;
    }

    if (identityData.size() != payloadLength || identityData.at(0) != 2) {
        LFLOG_ERROR << "Invalid identity ID payload";
        DsEngine::instance().reportStatus(tr("Invalid identity ID payload"),
                                          DsEngine::StatusLevel::ERROR);
        return;
    }

    data->cert = crypto::DsCert::createFromPubkey(identityData.mid(1, 32));
    const auto host = crypto::onionv3encode(identityData.mid(33, 35));
    auto portBytes = identityData.mid(68, 2);
    const auto port = qFromBigEndian(bytesToValue<quint16>(portBytes));
    data->address = (QString("onion:") + host + ".onion:"
                     + QString::number(port)).toUtf8();
    data->addMeMessage = args.value("addmeMessage").toString();
    data->autoConnect = args.value("autoConnect").toBool();
    data->notes = args.value("notes").toString();

    const auto probedHandle = data->cert->getHash().toByteArray();
    if (bioProbe_ && bioProbe_->getCert()
            && bioProbe_->getCert()->getHash().toByteArray() == probedHandle) {
        // Keep the probe connection separate. The received bio is copied into
        // the new record, then the normal contact lifecycle creates its own
        // connection after the dialog is accepted.
        data->bio = bioProbe_->getBio();
        data->bioHash = QCryptographicHash::hash(data->bio.toUtf8(),
                                                 QCryptographicHash::Sha256);
        data->avatar = bioProbe_->getAvatar();
    }

    try {
        // The probe deliberately uses a separate connection.  Close it before
        // starting the real contact connection; otherwise both connections
        // can race through the peer handshake and the Addme request may never
        // reach the unknown-peer notification path.
        cancelContactProbe();
        DsEngine::instance().getContactManager()->addContact(move(data));
    } catch (const std::exception& ex) {
        // The database intentionally rejects duplicate contacts for the same
        // identity.  This is user input, so it must not terminate the app.
        LFLOG_WARN << "Failed to add contact " << args.value("name").toString()
                   << ": " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to add contact: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::WARNING);
    }
}

QString Identity::getProbeBio() const noexcept
{
    return bioProbe_ ? bioProbe_->getBio() : QString{};
}

QImage Identity::getProbeAvatar() const noexcept
{
    return bioProbe_ ? bioProbe_->getAvatar() : QImage{};
}

bool Identity::hasProbeAvatar() const noexcept
{
    return bioProbe_ && !bioProbe_->getAvatar().isNull();
}

void Identity::cancelContactProbe()
{
    if (!bioProbe_) {
        return;
    }
    bioProbe_->disconnectFromContact(true);
    bioProbe_.reset();
    emit probeBioChanged();
}

void Identity::probeContact(const QString& suppliedIdentityId)
{
    cancelContactProbe();
    if (!isOnline()) {
        DsEngine::instance().reportStatus(
            tr("Connect to Tor before checking a contact's public bio"),
            DsEngine::StatusLevel::WARNING);
        return;
    }

    DsEngine::instance().reportStatus(
        tr("Checking contact for a public bio"), DsEngine::StatusLevel::INFO);

    auto identityId = suppliedIdentityId.trimmed();
    const auto separator = identityId.indexOf(':');
    if (separator > 0) {
        identityId = identityId.mid(separator + 1);
    }

    constexpr auto payloadLength = 1 + 32 + 35 + 2;
    QByteArray identityData;
    try {
        identityData = crypto::b58tobin_check<QByteArray>(
            identityId.toStdString(), payloadLength, {11, 176});
    } catch (const std::exception& ex) {
        DsEngine::instance().reportStatus(
            tr("Invalid identity ID: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::WARNING);
        return;
    }
    if (identityData.size() != payloadLength || identityData.at(0) != 2) {
        DsEngine::instance().reportStatus(tr("Invalid identity ID payload"),
                                          DsEngine::StatusLevel::WARNING);
        return;
    }

    auto data = make_unique<ContactData>();
    data->identity = getId();
    data->cert = crypto::DsCert::createFromPubkey(identityData.mid(1, 32));
    const auto host = crypto::onionv3encode(identityData.mid(33, 35));
    auto portBytes = identityData.mid(68, 2);
    const auto port = qFromBigEndian(bytesToValue<quint16>(portBytes));
    data->address = (QStringLiteral("onion:") + host + QStringLiteral(".onion:")
                     + QString::number(port)).toUtf8();

    bioProbe_ = make_shared<Contact>(-1, false, move(data));
    bioProbe_->setProbeOnly(true);
    connect(bioProbe_.get(), &Contact::bioChanged, this, [this]() {
        emit probeBioChanged();
        if (bioProbe_ && !bioProbe_->getBio().isEmpty()) {
            DsEngine::instance().reportStatus(
                tr("Public bio received"), DsEngine::StatusLevel::INFO);
        }
    });
    connect(bioProbe_.get(), &Contact::avatarChanged, this, [this]() {
        emit probeAvatarChanged();
    });
    bioProbe_->connectToContact();
}

void Identity::startService()
{
    try {
    if (data_.addressData.isEmpty()) {
        LFLOG_ERROR << "The identity " << getName()
                    << " has no address. Cannot go online.";
        DsEngine::instance().reportStatus(
            tr("Cannot start identity %1: no address").arg(getName()),
            DsEngine::StatusLevel::ERROR);
        return;

    }
    auto serviceData = DsEngine::fromJson(data_.addressData);
    serviceData.insert("key", DsEngine::instance().secretStore().torServiceKey(data_.uuid));
    getProtocolManager().startService(data_.uuid, data_.cert, serviceData);
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to start identity " << getName() << ": " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to start identity %1: %2").arg(getName(), QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    } catch (...) {
        LFLOG_ERROR << "Failed to start identity " << getName() << ": unknown exception";
    }
}

void Identity::stopService()
{
    try {
    disconnectContacts();
    getProtocolManager().stopService(data_.uuid);
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to stop identity " << getName() << ": " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to stop identity %1: %2").arg(getName(), QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    } catch (...) {
        LFLOG_ERROR << "Failed to stop identity " << getName() << ": unknown exception";
    }
}

void Identity::changeTransport()
{
    try {
     LFLOG_NOTICE << "Requesting a new Tor service for " << getName();
     DsEngine::instance().createNewTransport(
                 data_.name.toUtf8(),
                 data_.uuid);
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to change transport for " << getName() << ": " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to change transport for %1: %2").arg(getName(), QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    } catch (...) {
        LFLOG_ERROR << "Failed to change transport for " << getName() << ": unknown exception";
    }
}

void Identity::setNewTorService(const QString &address, int port, const QString privateKey)
{
    try {
    LFLOG_NOTICE << "Setting a new Tor service for " << getName();

    static const QRegularExpression addressPattern(
        QStringLiteral("^(?:onion:)?([a-z2-7]{56})(?:\\.onion)?:([1-9][0-9]{0,4})$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = addressPattern.match(address + ":" + QString::number(port));
    if (!match.hasMatch()) {
        throw std::invalid_argument("Invalid Tor v3 service address");
    }
    const auto serviceId = match.captured(1).toLower();
    const auto servicePort = match.captured(2).toUShort();
    if (servicePort == 0 || !crypto::onionv3isValid(serviceId.toUtf8())) {
        throw std::invalid_argument("Invalid Tor v3 service address");
    }
    const auto key = privateKey.trimmed().toUtf8();
    if (key.isEmpty()) {
        throw std::invalid_argument("Tor hidden service private key is required");
    }

    DsEngine::instance().secretStore().storeTorServiceKey(data_.uuid, key);

    QJsonDocument json {
        QJsonObject {
            {"type", "Tor hidden service"},
            {"service_id", serviceId},
            {"key_type", "ED25519-V3"},
            {"port", servicePort},
            {"address", serviceId + ".onion:" + QString::number(servicePort)}
        }
    };

    if (isOnline()) {
        stopService();
    }

    setAddressData(json.toJson(QJsonDocument::Compact));
    setAddress((serviceId + ".onion:" + QString::number(servicePort)).toUtf8());
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to set Tor service for " << getName() << ": " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to set Tor service for %1: %2").arg(getName(), QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    } catch (...) {
        LFLOG_ERROR << "Failed to set Tor service for " << getName() << ": unknown exception";
    }
}

ProtocolManager& Identity::getProtocolManager() {
    return DsEngine::instance().getProtocolMgr(
                ProtocolManager::Transport::TOR);
}

void Identity::onIncomingPeer(const std::shared_ptr<PeerConnection>& peer)
{
    if (auto contact = contactFromHash(peer->getPeerCert()->getHash().toByteArray())) {

        LFLOG_DEBUG << "Passing incoming connection "
                    << peer->getConnectionId().toString()
                    << " to Contact " << contact->getName()
                    << " on Identity " << getName();

        contact->onIncomingPeer(peer);
        return;
    }

    connect(peer.get(), &PeerConnection::addmeRequest,
            this, &Identity::onAddmeRequest);
    connect(peer.get(), &PeerConnection::receivedBioRequest,
            this, [this](const PeerReq& request) {
        if (getPublicBio()) {
            request.peer->sendBioResponse(request.requestId, getBio());
        } else {
            request.peer->sendBioResponse(request.requestId, {});
        }
    });
    connect(peer.get(), &PeerConnection::receivedAvatarRequest,
            this, [this](const PeerReq& request) {
        request.peer->sendAvatarResponse(request.requestId,
                                         getAvatar(), getPublicAvatar());
    });

    // At this point we leave the ownership and responsibility for the
    // connection at the protocol layer
    LFLOG_NOTICE << "incoming connection from unknow handle: " << peer->getPeerCert()->getB58PubKey();

    peer->authorize(true);
}

void Identity::onAddmeRequest(const PeerAddmeReq &req)
{
    LFLOG_TRACE << "Identity " << getName()
                << " received contact request from handle " << req.handle
                << ", request id " << req.requestId;

    if (auto contact = contactFromHandle(req.handle)) {
        LFLOG_TRACE << "Contact request from handle " << req.handle
                    << " matched an existing contact " << contact->getName();
        contact->onAddmeRequest(req);
        return;
    }

    LFLOG_TRACE << "Contact request from handle " << req.handle
                << " is for an unknown contact; relaying notification";

    // We need to trigger a signal that can be caught by the Notification system
    // in a higher API layer than the current library.
    // Whoever needs this signal will connect to a related signal in the
    // IdentityManager.
    DsEngine::instance().getIdentityManager()->relayNewContactRequest(this, req);

    if (req.avatarHash.size() == 32) {
        LFLOG_TRACE << "Relayed contact request from handle " << req.handle
                    << "; keeping request connection for avatar retrieval";
    } else {
        LFLOG_TRACE << "Relayed contact request from handle " << req.handle
                    << "; no avatar requested, closing request connection";
        req.peer->close();
    }
}

void Identity::onProcessOnlineLater()
{
    connectContacts();
}

void Identity::connectContacts()
{
    QSqlQuery query;
    query.prepare("SELECT uuid FROM contact WHERE identity=:id AND auto_connect=1");
    query.bindValue(":id", getId());
    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to fetch contact from hash: %1").arg(
                        query.lastError().text()));
    }

    while (query.next()) {
        auto contact = DsEngine::instance().getContactManager()->getContact(query.value(0).toUuid());

        // Connect to contacts with random delays to make it a tiny bit harder for
        // NSA, German intelligence and GRU to deduce what's going on,
        // based on based on the meta-date they collect from the transport
        // layer on the network.
        const auto delay = getRandomConnectDelay();

        LFLOG_DEBUG << "Identity " << getName()
                    << " will connect to Contact " << contact->getName()
                    << " in " << (delay / 1000) << " seconds.";

        auto uuid = contact->getUuid();
        QTimer::singleShot(delay, this, [this, uuid]() {
            // Get the contact again fromn the manager.
            // This will fail if the contact was deleted while the timer was running...
            if (auto contact = DsEngine::instance().getContactManager()->getContact(uuid)) {
                if (isOnline()
                      && !contact->isBlocked()
                      && contact->isAutoConnect()
                      && !contact->wasManuallyDisconnected()
                      && (contact->getOnlineStatus() == Contact::DISCONNECTED)
                      && ((contact->getState() == Contact::WAITING_FOR_ACCEPTANCE)
                       || (contact->getState() == Contact::ACCEPTED)
                       || (contact->getState() == Contact::PENDING))) {
                    contact->connectToContact();
                }
            }
        });
    }
}

int Identity::getRandomConnectDelay()
{
     static random_device rd;
     static mt19937 gen{rd()}; //Standard mersenne_twister_engine seeded with rd()
     static std::uniform_int_distribution<> dis(3000, 60000);
     return dis(gen);
}

void Identity::disconnectContacts()
{
    std::deque<Contact::ptr_t> contacts;
    for(const auto& it : connected_) {
        assert(it.second->contact);
        contacts.push_back(it.second->contact);
    }

    for(auto& contact : contacts) {
        LFLOG_DEBUG << "Identity " << getName()
                    << " is disconnected; disconnecting Contact: "
                    << contact->getName();
        contact->disconnectFromContact();
    }
}

void Identity::forAllContacts(const std::function<void (const Contact::ptr_t &)>& fn)
{
    auto contacts = getAllContacts();
    assert(fn);
    for(const auto& uuid : contacts) {
        if (auto contact = contactFromUuid(uuid)) {
            try {
                fn(contact);
            } catch(const std::exception& ex) {
                LFLOG_ERROR << "Caught exception from generic operation on Contact "
                            << contact->getName()
                            << " of Identity " << contact->getIdentity()->getName()
                            << ": " << ex.what();
            }
        }
    }
}

std::deque<QUuid> Identity::getAllContacts() const
{
    std::deque<QUuid> contacts;

    QSqlQuery query;
    query.prepare("SELECT uuid FROM contact WHERE identity=:id");
    query.bindValue(":id", getId());
    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to fetch contact from hash: %1").arg(
                        query.lastError().text()));
    }

    if (query.next()) {
        contacts.push_back(query.value(0).toUuid());
    }

    return contacts;
}

Contact::ptr_t Identity::contactFromHandle(const QString &handle)
{
    auto cert = crypto::DsCert::createFromPubkey(crypto::b58tobin_check<QByteArray>(
                                                     handle.toStdString(), 32, {249, 50}));

    return contactFromHash(cert->getHash().toByteArray());
}

Contact::ptr_t Identity::contactFromHash(const QByteArray &hash)
{
    QSqlQuery query;
    query.prepare("SELECT uuid FROM contact WHERE identity=:id AND hash=:hash");
    query.bindValue(":id", getId());
    query.bindValue(":hash", hash);
    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to fetch contact from hash: %1").arg(
                        query.lastError().text()));
    }

    if (query.next()) {
        return contactFromUuid(query.value(0).toUuid());
    }

    return {};
}

Contact::ptr_t Identity::contactFromUuid(const QUuid &uuid)
{
    if (auto contact = DsEngine::instance().getContactManager()->getContact(uuid)) {
        if (contact->getIdentityId() == getId()) {
            return contact;
        }
    }

    return {};
}

Conversation::ptr_t Identity::convesationFromHash(const QByteArray &hash)
{
    QSqlQuery query;
    query.prepare("SELECT uuid FROM conversation WHERE identity=:id AND hash=:hash");
    query.bindValue(":id", getId());
    query.bindValue(":hash", hash);
    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to fetch conversation from hash: %1").arg(
                        query.lastError().text()));
    }

    if (query.next()) {
        return DsEngine::instance().getConversationManager()->getConversation(query.value(0).toUuid());
    }

    return {};
}

void Identity::registerConnection(const Contact::ptr_t &contact)
{
    assert(contact);
    connected_[contact->getUuid()] = make_unique<Connected>(contact);
    assert(connected_[contact->getUuid()]->contact);

    LFLOG_TRACE << "Added contact #" << contact->getId() << " " << contact->getName()
                << " with uuid " << contact->getUuid().toString()
                << " for identity " << getName()
                << ". I now have " << connected_.size()
                << " active connections ";
}

void Identity::unregisterConnection(const QUuid &uuid)
{
    connected_.erase(uuid);

    LFLOG_TRACE << "Removed contact with uuid " << uuid.toString()
                << " for identity " << getName()
                << ". I now have " << connected_.size()
                << " active connections ";
}

int Identity::getId() const noexcept {
    return id_;
}

QString Identity::getName() const noexcept  {
    return data_.name;
}

void Identity::setName(const QString &name) {
    if (updateIf("name", name, data_.name, this, &Identity::nameChanged)) {
        forAllContacts([](const Contact::ptr_t& contact){
            if (contact->isOnline()) {
                contact->sendUserInfo();
            }
        });
    }
}

QByteArray Identity::getAddress() const noexcept {
    return data_.address;
}

void Identity::setAddress(const QByteArray &address) {
    updateIf("address", address, data_.address, this, &Identity::addressChanged);
}

QByteArray Identity::getAddressData() const noexcept {
    return data_.addressData;
}

void Identity::setAddressData(const QByteArray &data) {
    updateIf("address_data", data, data_.addressData, this, &Identity::addressDataChanged);
}

QString Identity::getNotes() const noexcept {
    return data_.notes;
}

void Identity::setNotes(const QString &notes) {
    updateIf("notes", notes, data_.notes, this, &Identity::notesChanged);
}

QImage Identity::getAvatar() const noexcept  {
    return data_.avatar;
}

QString Identity::getAvatarUrl() const noexcept
{
    if (avatarUrlChanging_) {
        return "";
    }

    return QStringLiteral("image://identity/%1?v=%2")
        .arg(data_.uuid.toString())
        .arg(avatarVersion_);
}

void Identity::setAvatar(const QImage &avatar) {
    const auto normalized = normalizeAvatar(avatar);
    if (updateIf("avatar", normalized, data_.avatar, this, &Identity::avatarChanged)) {
        ++avatarVersion_;
        avatarUrlChanging_ = true;
        emit avatarUrlChanged();
        avatarUrlChanging_ = false;
        emit avatarUrlChanged();

        forAllContacts([&normalized](const Contact::ptr_t& contact){
            if (contact->isOnline()) {
                contact->sendAvatar(normalized);
            }
        });
    }
}

bool Identity::isOnline() const noexcept {
    return online_;
}

void Identity::setOnline(const bool value) {
    if (value != online_) {
        online_ = value;
        emit onlineChanged();

        if (online_) {
            emit processOnlineLater();
        } else {
            disconnectContacts();
        }
    }
}

QUuid Identity::getUuid() const noexcept {
    return data_.uuid;
}

QByteArray Identity::getHash() const noexcept {
    return data_.hash;
}

QDateTime Identity::getCreated() const noexcept {
    return created_;
}

crypto::DsCert::ptr_t Identity::getCert() const noexcept {
    return data_.cert;
}

QByteArray Identity::getB58EncodedIdetity() const noexcept
{
    return DsEngine::getIdentityAsBase58(data_.cert, data_.address);
}

QByteArray Identity::getHandle() const noexcept
{
    return data_.cert->getB58PubKey();
}

bool Identity::isAutoConnect() const noexcept
{
    return data_.autoConnect;
}

void Identity::setAutoConnect(bool value)
{
    updateIf("auto_connect", value, data_.autoConnect, this, &Identity::autoConnectChanged);
}

void Identity::setAllowUtf8(bool value)
{
    if (value == data_.allowUtf8) return;
    data_.allowUtf8 = value;
    DsEngine::instance().settings().setValue(
        QStringLiteral("identity/%1/allowUtf8").arg(data_.uuid.toString(QUuid::WithoutBraces)), value);
    emit securityPreferencesChanged();
    forAllContacts([](const Contact::ptr_t& contact) { if (contact->isOnline()) contact->sendUserInfo(); });
}

void Identity::setAllowFiles(bool value)
{
    if (value == data_.allowFiles) return;
    data_.allowFiles = value;
    DsEngine::instance().settings().setValue(
        QStringLiteral("identity/%1/allowFiles").arg(data_.uuid.toString(QUuid::WithoutBraces)), value);
    emit securityPreferencesChanged();
    forAllContacts([](const Contact::ptr_t& contact) { if (contact->isOnline()) contact->sendUserInfo(); });
}

void Identity::setAllowMimeTypes(QStringList value)
{
    if (value == data_.allowMimeTypes) return;
    data_.allowMimeTypes = std::move(value);
    DsEngine::instance().settings().setValue(
        QStringLiteral("identity/%1/allowMimeTypes").arg(data_.uuid.toString(QUuid::WithoutBraces)),
        data_.allowMimeTypes);
    emit securityPreferencesChanged();
    forAllContacts([](const Contact::ptr_t& contact) { if (contact->isOnline()) contact->sendUserInfo(); });
}

void Identity::setAllowReactions(bool value)
{
    if (value == data_.allowReactions) return;
    data_.allowReactions = value;
    DsEngine::instance().settings().setValue(
        QStringLiteral("identity/%1/allowReactions").arg(data_.uuid.toString(QUuid::WithoutBraces)), value);
    emit securityPreferencesChanged();
    forAllContacts([](const Contact::ptr_t& contact) { if (contact->isOnline()) contact->sendUserInfo(); });
}

void Identity::setPublicBio(bool value)
{
    if (value == data_.publicBio) return;
    data_.publicBio = value;
    DsEngine::instance().settings().setValue(
        QStringLiteral("identity/%1/publicBio").arg(data_.uuid.toString(QUuid::WithoutBraces)), value);
    emit securityPreferencesChanged();
    forAllContacts([](const Contact::ptr_t& contact) { if (contact->isOnline()) contact->sendUserInfo(); });
}

void Identity::setBio(const QString& value)
{
    const auto limited = bio::sanitize(value);
    if (!bio::withinLimits(limited) || limited == data_.bio) {
        return;
    }
    data_.bio = limited;
    DsEngine::instance().settings().setValue(
        QStringLiteral("identity/%1/bio").arg(data_.uuid.toString(QUuid::WithoutBraces)),
        data_.bio);
    emit bioChanged();
    forAllContacts([](const Contact::ptr_t& contact) {
        if (contact->isOnline()) contact->sendUserInfo();
    });
}

void Identity::setPublicAvatar(bool value)
{
    if (value == data_.publicAvatar) return;
    data_.publicAvatar = value;
    DsEngine::instance().settings().setValue(
        QStringLiteral("identity/%1/publicAvatar").arg(data_.uuid.toString(QUuid::WithoutBraces)), value);
    emit securityPreferencesChanged();
    forAllContacts([](const Contact::ptr_t& contact) { if (contact->isOnline()) contact->sendUserInfo(); });
}

void Identity::addToDb() {
    QSqlQuery query;

    QByteArray avatar;
    if (!data_.avatar.isNull()) {
        QBuffer inBuffer( &avatar );
        inBuffer.open( QIODevice::WriteOnly );
        data_.avatar.save(&inBuffer, "PNG");
    }

    query.prepare("INSERT INTO identity ("
                  "uuid, hash, name, cert, address, address_data, notes, avatar, created, auto_connect"
                  ") VALUES ("
                  ":uuid, :hash, :name, :cert, :address, :address_data, :notes, :avatar, :created, :auto_connect"
                  ")");
    query.bindValue(":uuid", data_.uuid);
    query.bindValue(":hash", data_.hash);
    query.bindValue(":name", data_.name);
    query.bindValue(":cert", data_.cert->getSigningPubKey().toByteArray());
    query.bindValue(":address", data_.address);
    query.bindValue(":address_data", data_.addressData);
    query.bindValue(":notes", data_.notes);
    query.bindValue(":avatar", avatar);
    query.bindValue(":created", created_);
    query.bindValue(":auto_connect", data_.autoConnect);

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to add identity: %1").arg(
                        query.lastError().text()));
    }

    id_ = query.lastInsertId().toInt();

    LFLOG_INFO << "Added identity " << data_.name
               << " with uuid " << data_.uuid.toString()
               << " to the database with id " << id_;

}

void Identity::deleteFromDb() {
    if (id_ > 0) {
        auto db = QSqlDatabase::database();
        if (!db.transaction()) {
            throw Error(QStringLiteral("SQL Failed to start identity deletion: %1")
                            .arg(db.lastError().text()));
        }

        try {
            QSqlQuery query(db);
            QStringList filesToRemove;

            query.prepare("SELECT path FROM file WHERE identity_id=:id");
            query.bindValue(":id", id_);
            if (!query.exec()) {
                throw Error(QStringLiteral("SQL Failed to enumerate identity files: %1")
                                .arg(query.lastError().text()));
            }
            while (query.next()) {
                filesToRemove.append(query.value(0).toString());
            }

            // Identity deletion is recursive, but the legacy schema does not
            // declare cascading foreign keys for all identity-owned tables.
            query.prepare("DELETE FROM file WHERE identity_id=:id");
            query.bindValue(":id", id_);
            if (!query.exec()) {
                throw Error(QStringLiteral("SQL Failed to delete identity files: %1")
                                .arg(query.lastError().text()));
            }

            query.prepare("DELETE FROM notification WHERE identity=:id");
            query.bindValue(":id", id_);
            if (!query.exec()) {
                throw Error(QStringLiteral("SQL Failed to delete identity notifications: %1")
                                .arg(query.lastError().text()));
            }

            query.prepare("DELETE FROM message WHERE conversation_id IN "
                          "(SELECT id FROM conversation WHERE identity=:id)");
            query.bindValue(":id", id_);
            if (!query.exec()) {
                throw Error(QStringLiteral("SQL Failed to delete identity messages: %1")
                                .arg(query.lastError().text()));
            }

            query.prepare("DELETE FROM conversation WHERE identity=:id");
            query.bindValue(":id", id_);
            if (!query.exec()) {
                throw Error(QStringLiteral("SQL Failed to delete identity conversations: %1")
                                .arg(query.lastError().text()));
            }

            query.prepare("DELETE FROM contact WHERE identity=:id");
            query.bindValue(":id", id_);
            if (!query.exec()) {
                throw Error(QStringLiteral("SQL Failed to delete identity contacts: %1")
                                .arg(query.lastError().text()));
            }

            query.prepare("DELETE FROM identity WHERE id=:id");
            query.bindValue(":id", id_);
            if (!query.exec()) {
                throw Error(QStringLiteral("SQL Failed to delete identity: %1")
                                .arg(query.lastError().text()));
            }

            DsEngine::instance().secretStore().removeIdentityCertificate(data_.uuid);
            try {
            DsEngine::instance().secretStore().removeTorServiceKey(data_.uuid);
            } catch (const std::exception&) {
                // A service key may not exist for identities that never had a
                // transport.
            }
            DsEngine::instance().settings().remove(
                QStringLiteral("identity/%1").arg(data_.uuid.toString(QUuid::WithoutBraces)));

            if (!db.commit()) {
                throw Error(QStringLiteral("SQL Failed to commit identity deletion: %1")
                                .arg(db.lastError().text()));
            }

            for (const auto& path : filesToRemove) {
                (void)QFile::remove(path);
                (void)QFile::remove(path + QStringLiteral(".part"));
                (void)QFile::remove(path + QStringLiteral(".part.meta"));
            }
        } catch (...) {
            db.rollback();
            throw;
        }
    }
}


}} // namespaces
