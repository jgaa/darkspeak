#ifndef IDENTITIESMODEL_H
#define IDENTITIESMODEL_H

#include <QSettings>
#include <QSqlTableModel>
#include <QImage>
#include <QMetaType>

#include "ds/dscert.h"
#include "ds/dsengine.h"
#include "ds/identity.h"
#include "ds/model_util.h"

namespace ds {
namespace models {

class IdentitiesModel : public QSqlTableModel
{
    Q_OBJECT

    enum Roles {
        // Fields that don't exist in the database
        ONLINE_ROLE = Qt::UserRole + 100,
        HANDLE_ROLE
    };

    // Details kept in memory
    class ExtraInfo {
    public:
        using ptr_t = std::shared_ptr<ExtraInfo>;

        bool online = false;
        crypto::DsCert::ptr_t cert;
    };

public:

    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE QString getIdentityAsBase58(int row) const;
    Q_INVOKABLE void createNewTransport(int row);
    Q_INVOKABLE int row2Id(int row);

    IdentitiesModel(QSettings& settings);
    QUuid getUuidFromId(const int id) const;
    crypto::DsCert::ptr_t getCert(const QUuid& uuid) const;

protected:
    QSettings& settings_;

public slots:
    void createIdentity(QString name);
    void deleteIdentity(int index);
    void startService(int row);
    void stopService(int row);
    bool getOnlineStatus(int row) const;

private slots:
    void saveIdentity(const ds::core::Identity& data);
    void onServiceStarted(const QUuid& servive);
    void onServiceStopped(const QUuid& servive);
    void onServiceFailed(const QUuid& servive, const QByteArray& reason);
    void onTransportHandleReady(const core::TransportHandle& th);

    // QAbstractItemModel interface
public:
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    virtual QHash<int, QByteArray> roleNames() const override;

    bool identityExists(QString name) const;

private:
    int getIdFromRow(const int row) const;
    int getRowFromId(const int id) const;
    //QUuid getUuidFromRow(const int row) const;
    QUuid getUuidFromRow(const int row) const;
    int getIdFromUuid(const QUuid& uuid) const;
    int getRowFromUuid(const QUuid& uuid) const;
    ExtraInfo::ptr_t getExtra(const int row) const;
    ExtraInfo::ptr_t getExtra(const QUuid& uuid, bool createIfMissing = true) const;

    int h_id_ = {};
    int h_uuid_ = {};
    int h_hash_ = {};
    int h_name_ = {};
    int h_cert_ = {};
    int h_address_ = {};
    int h_address_data_ = {};
    int h_notes_= {};
    int h_avatar_ = {};
    int h_created_ = {};

    mutable std::map<QUuid, ExtraInfo::ptr_t> extras_;
};


}} // namespaces

#endif // IDENTITIESMODEL_H
