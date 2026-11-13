#pragma once
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariantList>
#include <stdexcept>

namespace ds::mcp {
class Error : public std::runtime_error {
  public:
    explicit Error(const char* code) : std::runtime_error(code) {}
};
// All access is confined to the application's Qt thread and existing database.
class Store {
  public:
    explicit Store(QSqlDatabase db);
    QSqlQuery query(const QString& sql, const QVariantList& values = {}) const;
    QSqlDatabase db() const {
        return db_;
    }

  private:
    QSqlDatabase db_;
};
} // namespace ds::mcp
