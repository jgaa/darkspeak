#ifndef BASE32_H
#define BASE32_H

#include <QByteArray>

namespace ds {
namespace crypto {

QByteArray onionv3decode(const QByteArray& src);
QByteArray onionv3encode(const QByteArray& src);
bool onionv3isValid(const QByteArray& src) noexcept;

}} // namespaces

#endif // BASE32_H
