#ifndef LOGMODEL_H
#define LOGMODEL_H

#include <deque>

#include <QSettings>
#include <QAbstractListModel>
#include <QImage>

#include "logfault/logfault.h"

namespace ds {
namespace models {

class LogModelHandler : public QObject, public logfault::Handler {
    Q_OBJECT
public:
    LogModelHandler(logfault::LogLevel level) : Handler(level) {}

    void LogMessage(const logfault::Message& msg) override;

signals:
    void message(QString message, logfault::LogLevel level);
};

class LogModel : public QAbstractListModel
{
    Q_OBJECT

    struct Item {
        Item(QString t, logfault::LogLevel ll)
            : text{std::move(t)}, level{ll} {}

        QString text;
        logfault::LogLevel level;
    };
public:
    LogModel(QSettings& settings);

// QAbstractItemModel interface
    QVariant data(const QModelIndex &index, int role) const override;
    int rowCount(const QModelIndex &parent) const override;

public slots:
    void log(QString text, logfault::LogLevel level);

private:
    QSettings& settings_;
    std::deque<std::unique_ptr<Item>> log_items_;
};


}} // namespaces

#endif // LOGMODEL_H
