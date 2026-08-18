
#include "ds/identitymanager.h"
#include "ds/errors.h"
#include "ds/dscert.h"
#include "ds/dsengine.h"


#include <QSqlQuery>
#include <QSqlError>
#include <QTimer>

#include "logfault/logfault.h"

using namespace std;
using namespace ds::crypto;

namespace ds {
namespace core {

IdentityManager::IdentityManager(QObject& parent)
    : QAbstractListModel(&parent)
{
    load();
}

void IdentityManager::load()
{
    QSqlQuery query;

    enum Fields {
        id, uuid, hash, name, cert, address, address_data, notes, avatar, created, auto_connect
    };

    query.prepare("SELECT "
                  "id, uuid, hash, name, cert, address, address_data, notes, avatar, created, auto_connect "
                  " FROM identity ");

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to add identity: %1").arg(
                        query.lastError().text()));
    }

    while(query.next()) {
        const auto identityUuid = query.value(Fields::uuid).toUuid();
        const auto publicKey = query.value(cert).toByteArray();
        const auto privateCertificate = DsEngine::instance().secretStore()
                                            .identityCertificate(identityUuid);
        auto cert = DsCert::create(privateCertificate);
        if (cert->getSigningPubKey().toByteArray() != publicKey) {
            throw Error("Identity certificate in the OS vault does not match the database");
        }
        IdentityData data;
        data.uuid = identityUuid;
        data.name = query.value(name).toString();
        data.hash = query.value(hash).toByteArray();
        data.address = query.value(address).toByteArray();
        data.addressData = query.value(address_data).toByteArray();
        data.notes = query.value(notes).toString();
        data.avatar = QImage::fromData(query.value(avatar).toByteArray(), "PNG");
        data.cert = cert;
        data.autoConnect = query.value(auto_connect).toBool();

        const auto prefix = QStringLiteral("identity/%1/").arg(
            identityUuid.toString(QUuid::WithoutBraces));
        const auto& settings = DsEngine::instance().settings();
        data.bio = settings.value(prefix + QStringLiteral("bio")).toString();
        data.allowUtf8 = settings.value(prefix + QStringLiteral("allowUtf8"), true).toBool();
        data.allowFiles = settings.value(prefix + QStringLiteral("allowFiles"), true).toBool();
        data.allowMimeTypes = settings.value(prefix + QStringLiteral("allowMimeTypes"),
                                              QStringList{QStringLiteral("*")}).toStringList();
        data.allowReactions = settings.value(prefix + QStringLiteral("allowReactions"), true).toBool();
        data.publicBio = settings.value(prefix + QStringLiteral("publicBio"), true).toBool();
        data.publicAvatar = settings.value(prefix + QStringLiteral("publicAvatar"), true).toBool();

        auto identity = new Identity{
                    *this,
                    query.value(id).toInt(),
                    false,
                    query.value(created).toDateTime(),
                    data};

        addIndex(identity, false);
    }

}

int IdentityManager::getRow(const Identity *identity) const
{
    int rowid = 0;
    for(const auto id : rows_) {
        if (id == identity) {
            return rowid;
        }
        ++rowid;
    }

    throw runtime_error("Identity not found");
}

Identity *IdentityManager::identityFromUuid(const QUuid &uuid) const
{
    auto it = uuids_.find(uuid);
    if (it == uuids_.end()) {
        return nullptr;
    }
    return it->second;
}

Identity *IdentityManager::identityFromRow(const int row) const
{
    if (row < 0 || row > static_cast<int>(rows_.size())) {
        return {};
    }

    return rows_[static_cast<size_t>(row)];
}

Identity *IdentityManager::identityFromId(const int id) const
{
    auto it = ids_.find(id);
    if (it == ids_.end()) {
        return nullptr;
    }
    return it->second;
}

Identity *IdentityManager::getCurrentIdentity() const noexcept
{
    if (current_ >= 0 && current_ < static_cast<int>(rows_.size())) {
        return rows_[static_cast<size_t>(current_)];
    }
    return nullptr;
}

void IdentityManager::setCurrentIdentity(int row)
{
    try {
    if (row != current_) {
        current_ = row;
        emit currentIdentityChanged();
    }
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to select current identity: " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to select identity: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    } catch (...) {
        LFLOG_ERROR << "Failed to select current identity: unknown exception";
    }
}

void IdentityManager::setCurrentIdentityById(const int id)
{
    if (auto identity = identityFromId(id)) {
        setCurrentIdentity(getRow(identity));
    }
}

void IdentityManager::createIdentity(const QmlIdentityReq *req)
{
    try {
    assert(req);
    if (!DsEngine::instance().isOnline()) {
        LFLOG_WARN << "Ignoring identity creation request while Tor is offline.";
        return;
    }

    IdentityData id;
    id.name = req->value.name;
    id.avatar = req->value.avatar;
    id.notes = req->value.notes;
    id.bio = req->value.bio;
    id.allowUtf8 = req->value.allowUtf8;
    id.allowFiles = req->value.allowFiles;
    id.allowMimeTypes = req->value.allowMimeTypes;
    id.allowReactions = req->value.allowReactions;
    id.publicBio = req->value.publicBio;
    id.publicAvatar = req->value.publicAvatar;
    id.uuid = req->value.uuid;
    id.autoConnect = req->autoConnect();
    id.cert = DsCert::create();
    id.hash = id.cert->getHash().toByteArray();

    if (!addIdentity(id)) {
        return;
    }

    auto name = id.name;
    auto uuid = id.uuid;

    DsEngine::instance().whenOnline([this, name, uuid]() { tryMakeTransport(name, uuid); });
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to create identity: " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to create identity: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    } catch (...) {
        LFLOG_ERROR << "Failed to create identity: unknown exception";
    }
}

void IdentityManager::relayNewContactRequest(Identity *identity, const PeerAddmeReq &req)
{
    try {
        LFLOG_TRACE << "Relaying contact request for identity " << identity->getName()
                    << " to notification listeners; handle " << req.handle
                    << ", request id " << req.requestId;
        emit newContactRequest(identity, req);
        LFLOG_TRACE << "Finished relaying contact request for identity "
                    << identity->getName();
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Caught exception: " << ex.what();
    }
}

void IdentityManager::disconnectAll()
{
    LFLOG_DEBUG << "Disconnecting all Identities.";
    auto tmpList = rows_;
    for(auto identity : tmpList) {
        identity->stopService();
    }
}

bool IdentityManager::exists(const QString &name) const
{
    QSqlQuery query;
    query.prepare("SELECT COUNT(*) FROM identity WHERE name=:name");
    query.bindValue(":name", name);

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to query: %1").arg(
                        query.lastError().text()));
    }

    query.next();
    return query.value(0).toInt() > 0;
}

void IdentityManager::tryMakeTransport(const QString &name, const QUuid& uuid)
{
    TransportHandleReq req{name, uuid};

    auto& engine = DsEngine::instance();

    try {
        engine.getProtocolMgr(ProtocolManager::Transport::TOR).createTransportHandle(req);
    } catch (const std::exception& ex) {
        LFLOG_DEBUG << "Failed to create transport for " << name
                 << " (will try again later): "
                 << ex.what();

        if (engine.isOnline()) {
            QTimer::singleShot(1000, this, [this, name, uuid]() {
                DsEngine::instance().whenOnline([this, name, uuid]() { tryMakeTransport(name, uuid); });
            });

            // TODO: Fail after n retries
        } else {
            engine.whenOnline([this, name, uuid]() { tryMakeTransport(name, uuid); });
        }
    }
}

int IdentityManager::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return static_cast<int>(rows_.size());
}

QVariant IdentityManager::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return {};
    }

    if (role != Qt::DisplayRole) {
        return {};
    }

    return QVariant::fromValue<ds::core::Identity *>(
                rows_.at(static_cast<size_t>(index.row())));
}

QHash<int, QByteArray> IdentityManager::roleNames() const
{
    static const QHash<int, QByteArray> names = {
        {Qt::DisplayRole, "identity"}
    };

    return names;
}

void IdentityManager::onOnline()
{
    for(auto identity : rows_) {
        if (identity->isAutoConnect() && !identity->isOnline() && !identity->getAddress().isEmpty()) {
            identity->startService();
        }
    }
}

Identity *IdentityManager::addIdentity(const IdentityData &data)
{
    const QDateTime when = QDateTime::fromSecsSinceEpoch((QDateTime::currentSecsSinceEpoch() / 60) * 60 );
    auto identity = new Identity{
                *this,
                -1,
                false,
                when,
                data};

    try {
        DsEngine::instance().secretStore().storeIdentityCertificate(
            data.uuid, data.cert->getCert().toByteArray());
        identity->addToDb();
    } catch (const std::exception& ex) {
        try {
            DsEngine::instance().secretStore().removeIdentityCertificate(data.uuid);
        } catch (const std::exception&) {
            // Preserve the original database error; startup will fail closed
            // if an orphaned secret is ever encountered.
        }
        LFLOG_ERROR << "Failed to add identity with name '"
                    << identity->getName()
                    << "' to the database: " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to add identity: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
        delete identity;
        return nullptr;
    }

    addIndex(identity, true);
    return identity;
}

void IdentityManager::removeIdentity(const QUuid &uuid)
{
    if (auto identity = identityFromUuid(uuid)) {
        try {
            identity->deleteFromDb();
        } catch (const std::exception& ex) {
            LFLOG_ERROR << "Failed to delete identity " << uuid.toString()
                        << ": " << ex.what();
            DsEngine::instance().reportStatus(
                tr("Failed to delete identity: %1").arg(QString::fromUtf8(ex.what())),
                DsEngine::StatusLevel::ERROR);
            return;
        }

        removeIndex(identity, true);
        delete identity;
        endRemoveRows();
    }
}

void IdentityManager::onIncomingPeer(const std::shared_ptr<PeerConnection>& peer)
{
    LFLOG_DEBUG << "Connection from peer " << peer->getPeerCert()->getB58PubKey()
                << " to identity " << peer->getIdentityId().toString();

    if (auto target = identityFromUuid(peer->getIdentityId())) {
        target->onIncomingPeer(peer);
    }
}

void IdentityManager::addIndex(Identity *identity, const bool notify)
{
    assert(identity != nullptr);

    {
        // Find the insert point for the identity in rows
        // We sort alphabetically by name
        auto it = rows_.begin();
        int rowid = 0;
        for(; it < rows_.end(); ++it, ++rowid) {
            if (QString::compare(identity->getName(), (*it)->getName(), Qt::CaseInsensitive) <= 0) {
                break;
            }
        }

        if (notify) {
            beginInsertRows({}, rowid, rowid);
        }

        rows_.insert(it, identity);
    }

    ids_[identity->getId()] = identity;
    uuids_[identity->getUuid()] = identity;

    if (notify) {
        endInsertRows();
    }
}

void IdentityManager::removeIndex(Identity *identity, const bool notify)
{
    ids_.erase(identity->getId());
    uuids_.erase(identity->getUuid());

    const auto rowid = getRow(identity);

    if (notify) {
        beginRemoveRows({}, rowid, rowid);
    }
    rows_.erase(rows_.begin() + rowid);
}


}} // namespaces
