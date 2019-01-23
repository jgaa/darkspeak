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
    };

public:

    IdentitiesModel(QSettings& settings);


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
    void onServiceStarted(const QByteArray& id);
    void onServiceStopped(const QByteArray& id);
    void onServiceFailed(const QByteArray& id, const QByteArray& reason);

    // QAbstractItemModel interface
public:
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    //QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
   // Qt::ItemFlags flags(const QModelIndex &index) const override;
    virtual QHash<int, QByteArray> roleNames() const override;

    bool identityExists(QString name) const;

private:
    QByteArray getIdFromRow(const int row) const;
    int getRowFromId(const QByteArray& id) const;
    ExtraInfo::ptr_t getExtra(const int row) const;
    ExtraInfo::ptr_t getExtra(const QByteArray& id) const;

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

    mutable std::map<QByteArray, ExtraInfo::ptr_t> extras_;
};


}} // namespaces

#endif // IDENTITIESMODEL_H
