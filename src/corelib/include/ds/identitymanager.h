#ifndef IDENTITYMANAGER_H
#define IDENTITYMANAGER_H

#include <deque>
#include <QAbstractListModel>

#include "ds/identity.h"

namespace ds {
namespace core {

/*! Object based manager for all the identities.
 *
 * Exposes an QAbstractListModel for the identities.
 *
 */

class IdentityManager : public QAbstractListModel
{
    Q_OBJECT
public:
    IdentityManager(QObject& parent);

    Q_PROPERTY(Identity * current READ getCurrentIdentity NOTIFY currentIdentityChanged)

    /*! Load all the identities from the database */
    void load();

    int getRow(const Identity *) const;
    Identity *addIdentity(const IdentityData& data);
    

    // Returns nullptr on failure
    Q_INVOKABLE Identity *identityFromUuid(const QUuid& uuid) const;
    Q_INVOKABLE Identity *identityFromRow(const int row) const;
    Q_INVOKABLE Identity *identityFromId(const int id) const;
    Q_INVOKABLE Identity *getCurrentIdentity() const noexcept;
    Q_INVOKABLE void setCurrentIdentity(int row);
    Q_INVOKABLE void createIdentity(const QmlIdentityReq *req);

    void relayNewContactRequest(Identity *identity, const core::PeerAddmeReq &req);
    void disconnectAll();

    // QAbstractItemModel interface
public:
    int rowCount(const QModelIndex &parent) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void onOnline();

signals:
    void currentIdentityChanged();
    void newContactRequest(Identity *identity, const core::PeerAddmeReq &req);

public slots:
    void removeIdentity(const QUuid& uuid);
    void onIncomingPeer(const std::shared_ptr<PeerConnection>& peer);

private:
    void addIndex(Identity *identity, bool notify);
    void removeIndex(Identity *identity, const bool notify);
    void tryMakeTransport(const QString &name, const QUuid& uuid);

    // Fast lookups for identities
    std::deque<Identity *> rows_; // As returned by data(row)
    std::map<int, Identity *> ids_;
    std::map<QUuid, Identity *> uuids_;
    int current_ = -1;

};

}} // namespaces

Q_DECLARE_METATYPE(ds::core::IdentityManager *)

#endif // IDENTITYMANAGER_H
