#ifndef DS_CORE_ERRORS_H
#define DS_CORE_ERRORS_H

#include <stdexcept>
#include <QString>

namespace ds {
namespace core {

struct Error : public std::runtime_error
{
    explicit Error(const char *what) : std::runtime_error(what) {}
    explicit Error(const QString& what) : std::runtime_error(what.toStdString()) {}
};

struct ExistsError : public Error
{
    explicit ExistsError(const char *what) : Error(what) {}
    explicit ExistsError(const QString& what) : Error(what) {}
};

struct NotFoundError : public Error
{
    explicit NotFoundError(const char *what) : Error(what) {}
    explicit NotFoundError(const QString& what) : Error(what) {}
};

struct ParseError : public Error
{
    explicit ParseError(const char *what) : Error(what) {}
    explicit ParseError(const QString& what) : Error(what) {}
};

struct OfflineError : public Error
{
    explicit OfflineError(const char *what) : Error(what) {}
    explicit OfflineError(const QString& what) : Error(what) {}
};

}} // namespaces

#endif // DS_CORE_ERRORS_H
