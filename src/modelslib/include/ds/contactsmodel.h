#ifndef CONTACTSMODEL_H
#define CONTACTSMODEL_H

#include <memory>

#include <QSettings>
#include <QImage>
#include <QMetaType>
#include <QUuid>
#include <QAbstractSocket>
#include <QAbstractListModel>

#include "ds/contact.h"
#include "ds/identitymanager.h"
#include "ds/contactmanager.h"
#include "ds/protocolmanager.h"
#include "ds/dsengine.h"

namespace ds {
namespace models {

/*! Simple ListModel to proxy the contacts for an identity,
 *
 * Sorted by name
 */

class ContactsModel : public QAbstractListModel
{
    Q_OBJECT

    struct Row {
        Row(QUuid&& uuidVal) : uuid{std::move(uuidVal)} {}

        mutable core::Contact::ptr_t contact;
        QUuid uuid;
    };

    using rows_t = std::deque<Row>;
public:

    ContactsModel(QObject& parent);

    // Set the identity to work with
    Q_INVOKABLE void setIdentity(const QUuid& uuid);

public slots:
    void onContactAdded(const core::Contact::ptr_t& contact);
    void onContactDeleted(const QUuid& contact);

    // QAbstractItemModel interface
public:
    int rowCount(const QModelIndex &parent) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    void queryRows(rows_t& rows);

    rows_t rows_;
    core::IdentityManager& identityManager_;
    core::ContactManager& contactManager_;
    core::Identity *identity_ = nullptr; // Active identity
};

}} // namespaces


Q_DECLARE_METATYPE(ds::models::ContactsModel *)

#endif // CONTACTSMODEL_H
