#ifndef AVATARHASH_H
#define AVATARHASH_H

#include <QDataStream>
#include <QCryptographicHash>
#include <QIODevice>
#include <QImage>

namespace ds {
namespace core {

inline QByteArray avatarHash(const QImage &avatar)
{
    if (avatar.isNull()) {
        return {};
    }

    QByteArray canonical;
    QDataStream stream(&canonical, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint32(avatar.width()) << quint32(avatar.height());

    for (int y = 0; y < avatar.height(); ++y) {
        for (int x = 0; x < avatar.width(); ++x) {
            const auto pixel = avatar.pixel(x, y);
            stream << quint8(qRed(pixel))
                   << quint8(qGreen(pixel))
                   << quint8(qBlue(pixel));
        }
    }

    return QCryptographicHash::hash(canonical, QCryptographicHash::Sha256);
}

}} // namespace ds::core

#endif // AVATARHASH_H
