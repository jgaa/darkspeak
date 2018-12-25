#include "ds/conversationsmodel.h"
#include "ds/dsengine.h"
#include "ds/errors.h"
#include "ds/model_util.h"
#include "ds/strategy.h"

#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QSqlRecord>
#include <QUuid>

#include "logfault/logfault.h"

using namespace std;
using namespace ds::core;

namespace ds {
namespace models {


ConversationsModel::ConversationsModel(QSettings& settings)
    : settings_{settings}
{
    setTable("conversation");
    setSort(fieldIndex("name"), Qt::AscendingOrder);
    setEditStrategy(QSqlTableModel::OnFieldChange);

    h_id_ = fieldIndex("id");
    h_identity_ = fieldIndex("identity");
    h_type_ = fieldIndex("type");
    h_name_ = fieldIndex("name");
    h_uuid_ = fieldIndex("uuid");
    h_key_ = fieldIndex("key");
    h_participants_ = fieldIndex("participants");
    h_topic_ = fieldIndex("topic");
}

bool ConversationsModel::keyExists(QByteArray key) const
{
    LFLOG_DEBUG << "Checking if key exists: " << key.toBase64();

    QSqlQuery query;
    query.prepare("SELECT 1 from conversation where key=:key");
    query.bindValue(":key", key);
    if(!query.exec()) {
        throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
    }
    return query.next();
}

}} // namespaces
