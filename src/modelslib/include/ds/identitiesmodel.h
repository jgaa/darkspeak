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
public:

    IdentitiesModel(QSettings& settings);

protected:
    QSettings& settings_;

public slots:
    void createIdentity(const ds::core::Identity& data);

    // QAbstractItemModel interface
public:
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    bool identityExists(QString name) const;

private:
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
};


}} // namespaces

#endif // IDENTITIESMODEL_H
