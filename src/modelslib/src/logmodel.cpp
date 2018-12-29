
#include <memory>
#include <QDateTime>

#include "ds/logmodel.h"

using namespace  std;

namespace ds {
namespace models {

LogModel::LogModel(QSettings &settings)
    : settings_{settings}
{
    auto handler = make_unique<LogModelHandler>(logfault::LogLevel::DEBUGGING);
    connect(handler.get(), &LogModelHandler::message, this, &LogModel::log);
    logfault::LogManager::Instance().AddHandler(move(handler));
}

QVariant LogModel::data(const QModelIndex &index, int role) const
{
    if (index.row() >= static_cast<int>(log_items_.size())) {
        return {};
    }

    if (role == Qt::DisplayRole) {
        const auto& li = log_items_[static_cast<size_t>(index.row())];
        return li->text;
    } else if (role == Qt::DecorationRole) {
        // TODO: Add icon
    }

    return {};
}

int LogModel::rowCount(const QModelIndex &/*parent*/) const
{
    return static_cast<int>(log_items_.size());
}

void LogModel::log(QString text, logfault::LogLevel level)
{
    auto item = make_unique<Item>(text, level);

    if (log_items_.size() > 100) {
        beginRemoveRows({}, 0, 0);
        log_items_.erase(log_items_.begin());
        endRemoveRows();
    }

    const auto row = static_cast<int>(log_items_.size());
    beginInsertRows({}, row, row);
    log_items_.push_back(move(item));
    endInsertRows();
}

void LogModelHandler::LogMessage(const logfault::Message &msg)
{
    std::ostringstream out;
    PrintMessage(out, msg);
    auto str = out.str();
    auto text = QString::fromUtf8(str.c_str());

    emit message(QString::fromUtf8(str.c_str()), msg.level_);
}


}} // namespaces
