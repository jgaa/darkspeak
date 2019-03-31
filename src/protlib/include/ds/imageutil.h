#ifndef IMAGEUTIL_H
#define IMAGEUTIL_H

#include <QImage>
#include <QJsonObject>

namespace ds {
namespace prot {

QJsonObject toJson(const QImage& image);
QImage toQimage(const QJsonObject& object);

}} // namespaces

#endif // IMAGEUTIL_H
