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

    connect(DsEngine::instance().getContactManager(),
            &ContactManager::contactAdded,
            this, &ContactsModel::onContactAdded);

    connect(DsEngine::instance().getContactManager(),
            &ContactManager::contactDeleted,
            this, &ContactsModel::onContactDeleted);
}

void ContactsModel::setSearch(const QString &text)
{
    const auto search = text.left(128).trimmed();
    if (search == search_) return;
    const auto previous = search_;
    search_ = search;
    try { reload(); }
    catch (const std::exception &) {
        search_ = previous;
        DsEngine::instance().reportStatus(tr("Unable to search contacts."), DsEngine::ERROR);
    }
    emit searchChanged();
}

void ContactsModel::setIdentity(const QUuid &uuid)
{
    try {
    identity_ = identityManager_.identityFromUuid(uuid);
    reload();
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to select identity contacts: " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to load contacts: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    }
}

void ContactsModel::reload()
{
    rows_t next;
    if (identity_ != nullptr) queryRows(next);
    beginResetModel();
    rows_ = std::move(next);
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
    Q_UNUSED(uuid);
    reload();
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

        if (!r.contact) {
            return {};
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
    QString sql = QStringLiteral("SELECT uuid FROM contact WHERE identity=:identity");
    // A default QString binds as SQL NULL, not ''. With no search, omit the
    // predicate entirely so both null and empty input show every contact.
    if (!search_.isEmpty()) {
        sql += QStringLiteral(" AND (instr(lower(name),lower(:search)) > 0 OR instr(lower(nickname),lower(:search)) > 0)");
    }
    sql += QStringLiteral(" ORDER BY LOWER(name)");
    query.prepare(sql);
    query.bindValue(":identity", identity_->getId());
    if (!search_.isEmpty()) query.bindValue(":search", search_);

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
