
#include <memory>
#include <QDateTime>

#include "ds/logutil.h"
#include "ds/logmodel.h"

using namespace  std;

namespace ds {
namespace models {

void LogModel::installHandler(logfault::LogLevel level)
{
    if (level == logfault::LogLevel::DISABLED) {
        return;
    }

    auto handler = make_unique<LogModelHandler>(level);
    connect(handler.get(), &LogModelHandler::message, this, &LogModel::log);
    logfault::LogManager::Instance().AddHandler(std::move(handler));
}

QVariant LogModel::data(const QModelIndex &index, int role) const
{
    if (index.row() >= static_cast<int>(log_items_.size())) {
        return {};
    }

    if (role == Qt::DisplayRole) {
        const auto& li = log_items_[static_cast<size_t>(index.row())];
        return li->text;
    }

    if (role == Qt::UserRole + 1) {
        const auto& li = log_items_[static_cast<size_t>(index.row())];
        return static_cast<int>(li->level);
    }

    return {};
}

int LogModel::rowCount(const QModelIndex &/*parent*/) const
{
    return static_cast<int>(log_items_.size());
}

QHash<int, QByteArray> LogModel::roleNames() const
{
    return {{Qt::DisplayRole, "display"}, {Qt::UserRole + 1, "level"}};
}

void LogModel::log(QString text, logfault::LogLevel level)
{
    auto item = make_unique<Item>(text, level);

    while (log_items_.size() >= 1000) {
        beginRemoveRows({}, 0, 0);
        log_items_.erase(log_items_.begin());
        endRemoveRows();
    }

    const auto row = static_cast<int>(log_items_.size());
    beginInsertRows({}, row, row);
    log_items_.push_back(move(item));
    endInsertRows();
}

void LogModelHandler::LogMessage(const logfault::Message &msg) noexcept
{
    std::ostringstream out;
    PrintMessage(out, msg);
    auto str = out.str();

    emit message(QString::fromUtf8(str.c_str()), msg.level_);
}


}} // namespaces
