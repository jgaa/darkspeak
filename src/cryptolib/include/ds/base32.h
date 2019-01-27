#ifndef BASE32_H
#define BASE32_H

#include <QByteArray>

namespace ds {
namespace crypto {

QByteArray onion16decode(const QByteArray& src);
QByteArray onion16encode(const QByteArray& src);

}} // namespaces

#endif // BASE32_H
