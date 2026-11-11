#pragma once
#include <QSortFilterProxyModel>

namespace ds::models {
class LogFilterModel final : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY filterChanged)
    Q_PROPERTY(int maximumLevel READ maximumLevel WRITE setMaximumLevel NOTIFY filterChanged)
public:
    explicit LogFilterModel(QObject *parent = nullptr) : QSortFilterProxyModel(parent) { setDynamicSortFilter(true); }
    QString search() const { return search_; }
    int maximumLevel() const { return maximum_level_; }
    void setSearch(const QString &text);
    void setMaximumLevel(int value);
signals:
    void filterChanged();
protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
private:
    QString search_;
    int maximum_level_ = 6;
};
}
