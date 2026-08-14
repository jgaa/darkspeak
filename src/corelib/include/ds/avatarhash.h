#ifndef AVATARHASH_H
#define AVATARHASH_H

#include <QCryptographicHash>
#include <QImage>

namespace ds {
namespace core {

inline QByteArray avatarHash(const QImage &avatar)
{
    if (avatar.isNull()) {
        return {};
    }

    const auto normalized = avatar.convertToFormat(QImage::Format_RGB888);
    QByteArray canonical;
    canonical.reserve(normalized.width() * normalized.height() * 3);
    QByteArray red;
    QByteArray green;
    QByteArray blue;
    red.resize(normalized.width() * normalized.height());
    green.resize(red.size());
    blue.resize(red.size());
    for (int y = 0; y < normalized.height(); ++y) {
        const auto *line = normalized.constScanLine(y);
        for (int x = 0; x < normalized.width(); ++x) {
            const auto index = y * normalized.width() + x;
            red[index] = line[3 * x];
            green[index] = line[3 * x + 1];
            blue[index] = line[3 * x + 2];
        }
    }
    canonical += red;
    canonical += green;
    canonical += blue;
    return QCryptographicHash::hash(canonical, QCryptographicHash::Sha256);
}

}} // namespace ds::core

#endif // AVATARHASH_H
