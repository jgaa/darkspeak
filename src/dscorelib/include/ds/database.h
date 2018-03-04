#ifndef DATABASE_H
#define DATABASE_H

#include <stdexcept>

#include <QObject>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlDriver>
#include <QSqlError>
#include <QSqlQuery>

namespace ds {
namespace core {

class Database : public QObject
{
    Q_OBJECT
public:
    struct Error : public std::runtime_error
    {
        explicit Error(const char *what) : std::runtime_error(what) {}
        explicit Error(const QString& what) : std::runtime_error(what.toStdString()) {}
    };

    Database(QSettings& settings);
    ~Database();

    enum DsTable {
        DS_VERSION = 0
    };

signals:

public slots:

protected:
    void createDatabase();
    void exec(const char *sql);

    static constexpr int currentVersion = 1;
    QSqlDatabase db_;
    QSettings& settings_;
};

}} // namespaces

#endif // DATABASE_H
