#ifndef AVATARUTIL_H
#define AVATARUTIL_H

#include <algorithm>
#include <cmath>

#include <QImage>

namespace ds {
namespace core {

constexpr int MaxAvatarDimension = 128;

inline QImage normalizeAvatar(const QImage &avatar)
{
    if (avatar.isNull()
        || (avatar.width() <= MaxAvatarDimension
            && avatar.height() <= MaxAvatarDimension)) {
        return avatar;
    }

    return avatar.scaled(MaxAvatarDimension, MaxAvatarDimension,
                         Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

/**
 * Return an avatar that is safe to render directly in a circular frame.
 *
 * QML's Rectangle radius only changes the painted background; it does not
 * clip child items to that radius.  Make the mask part of the image instead,
 * so the result is independent of the scene graph and Qt GraphicalEffects
 * version in use by the application.
 */
inline QImage circularAvatar(const QImage &avatar)
{
    if (avatar.isNull()) {
        return {};
    }

    const auto side = std::min(avatar.width(), avatar.height());
    if (side <= 0) {
        return {};
    }

    const auto scaled = avatar.scaled(side, side, Qt::KeepAspectRatioByExpanding,
                                      Qt::SmoothTransformation);
    const auto square = scaled.copy((scaled.width() - side) / 2,
                                    (scaled.height() - side) / 2,
                                    side, side)
                              .convertToFormat(QImage::Format_ARGB32);

    auto masked = square;
    const auto center = (side - 1) / 2.0;
    const auto radius = side / 2.0;
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const auto dx = x - center;
            const auto dy = y - center;
            const auto distance = std::sqrt(dx * dx + dy * dy);
            // Keep the mask binary.  Partially transparent edge pixels make
            // the white portions of the placeholder SVG halo against a
            // colored QML background.
            if (distance > radius) {
                auto color = masked.pixelColor(x, y);
                color.setAlpha(0);
                masked.setPixelColor(x, y, color);
            }
        }
    }
    return masked;
}

}} // namespace ds::core

#endif // AVATARUTIL_H
