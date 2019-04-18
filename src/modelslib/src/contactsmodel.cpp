#include <cassert>

#include "ds/contactsmodel.h"
#include "ds/dsengine.h"
#include "ds/errors.h"
#include "ds/model_util.h"
#include "ds/base58.h"
#include "ds/strategy.h"
#include "ds/manager.h"

#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>
#include <QSqlError>

#include "logfault/logfault.h"

using namespace std;
using namespace ds::core;

namespace ds {
namespace models {

ContactsModel::ContactsModel(QObject& parent)
    : QAbstractListModel{&parent}
    , identityManager_{*DsEngine::instance().getIdentityManager()}
    , contactManager_{*DsEngine::instance().getContactManager()}
{

    // TODO: Connect to add / delete contact signals

    connect(DsEngine::instance().getContactManager(),
            &ContactManager::contactAdded,
            this, &ContactsModel::onContactAdded);
}

void ContactsModel::setIdentity(const QUuid &uuid)
{
    beginResetModel();

    rows_.clear();
    identity_ = identityManager_.identityFromUuid(uuid);

    if (identity_ != nullptr) {
        queryRows(rows_);
    }

    endResetModel();
}


void ContactsModel::onContactAdded(const Contact::ptr_t &contact)
{
    if (!identity_) {
        return;
    }

    if (contact->getIdentity() != identity_) {
        return;
    }

    rows_t r;
    queryRows(r);

    // Assume that the r == rows_, except for one added item
    // Just find the insert point an add it

    const auto key = contact->getUuid();
    int rowid = 0;
    auto current_it = rows_.begin();
    for(auto &row : r) {
        if (row.uuid == key) {

            beginInsertRows({}, rowid, rowid);

            rows_.emplace(current_it, contact->getUuid());

            endInsertRows();
            break;
        }
        ++rowid;
        ++current_it;
    }
}

void ContactsModel::onContactDeleted(const QUuid &uuid)
{
    int rowid = 0;
    for(auto it = rows_.begin(); it != rows_.end(); ++it, ++rowid) {
        if (it->uuid == uuid) {
            beginRemoveRows({}, rowid, rowid);
            rows_.erase(it);
            endRemoveRows();
            break;
        }
    }
}

int ContactsModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return static_cast<int>(rows_.size());
}

QVariant ContactsModel::data(const QModelIndex &ix, int role) const
{
    if (ix.isValid() && ix.column() == 0 && role == Qt::DisplayRole) {
        auto &r = rows_.at(static_cast<size_t>(ix.row()));

        // Lazy loading
        if (!r.contact) {
            r.contact = contactManager_.getContact(r.uuid);
        }

        assert(identity_ != nullptr);
        assert(identity_ == r.contact->getIdentity());

        return QVariant::fromValue<ds::core::Contact *>(r.contact.get());
    }

    return {};
}

QHash<int, QByteArray> ContactsModel::roleNames() const
{
    static const QHash<int, QByteArray> names = {
        {Qt::DisplayRole, "contact"},
    };

    return names;
}

void ContactsModel::queryRows(rows_t &rows)
{
    if (!identity_) {
        return;
    }
    QSqlQuery query;
    query.prepare("SELECT uuid FROM contact WHERE identity=:identity ORDER BY LOWER(NAME)");
    query.bindValue(":identity", identity_->getId());

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to query contacts: %1").arg(
                        query.lastError().text()));
    }

    // Populate
    while(query.next()) {
        rows.emplace_back(query.value(0).toUuid());
    }
}

}} // namespaces

