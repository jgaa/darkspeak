#include "ds/logfiltermodel.h"
#include <algorithm>
namespace ds::models {
void LogFilterModel::setSearch(const QString &text)
{
    const auto value = text.left(256);
    if (search_ == value) return;
    beginFilterChange();
    search_ = value;
    endFilterChange(Direction::Rows);
    emit filterChanged();
}
void LogFilterModel::setMaximumLevel(int value)
{
    value = std::clamp(value, 1, 6);
    if (value == maximum_level_) return;
    beginFilterChange();
    maximum_level_ = value;
    endFilterChange(Direction::Rows);
    emit filterChanged();
}
bool LogFilterModel::filterAcceptsRow(int row, const QModelIndex &parent) const
{
    const auto index = sourceModel()->index(row, 0, parent);
    return sourceModel()->data(index, Qt::UserRole + 1).toInt() <= maximum_level_
        && sourceModel()->data(index, Qt::DisplayRole).toString().contains(search_, Qt::CaseInsensitive);
}
}
