
#include <QQmlEngine>

#include "include/ds/imageprovider.h"
#include "ds/avatarutil.h"

#include "logfault/logfault.h"

using namespace std;

namespace ds {
namespace models {

namespace {

QImage maskToCircle(QImage image)
{
    return core::circularAvatar(image);
}

}

ImageProvider::ImageProvider(QString name,
                             ImageProvider::provider_t provider)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , provider_{move(provider)}, name_{move(name)}
{
}

QQmlImageProviderBase::ImageType ImageProvider::imageType() const
{
    return QQmlImageProviderBase::Image;
}

QImage ImageProvider::requestImage(const QString &id, QSize *size,
                                   const QSize &requestedSize)
{
    QString key = QUrl::fromPercentEncoding(id.toUtf8());
    // The query is a cache-busting revision used by QML, not part of the UUID.
    const auto query = key.indexOf('?');
    if (query >= 0) {
        key.truncate(query);
    }

    LFLOG_TRACE << "Requesting image (" << getName() << ") " << key;

    auto image = provider_(key);
    const auto providerReturnedNull = image.isNull();
    LFLOG_DEBUG << "Image provider callback (" << getName() << ") " << key
                << ": null=" << providerReturnedNull
                << ", size=" << image.width() << "x" << image.height()
                << ", format=" << static_cast<int>(image.format());
    if (image.isNull()) {
        image.load(defaultImage_);
        LFLOG_DEBUG << "Image provider fallback (" << getName() << ") " << key
                    << ": path=" << defaultImage_
                    << ", loaded=" << !image.isNull()
                    << ", size=" << image.width() << "x" << image.height()
                    << ", format=" << static_cast<int>(image.format());
    }

    QImage copy;

    if (!requestedSize.isValid()) {
        copy = image;
    } else {
        copy = image.scaled(requestedSize.width(),
                             requestedSize.height(),
                             Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }

    if ((getName() == QStringLiteral("contact")
         || getName() == QStringLiteral("identity")) && !copy.isNull()) {
        copy = maskToCircle(std::move(copy));
    }

    LFLOG_DEBUG << "Image provider delivered (" << getName() << ") " << key
                << ": fallback=" << providerReturnedNull
                << ", requested=" << requestedSize.width() << "x"
                << requestedSize.height()
                << ", size=" << copy.width() << "x" << copy.height()
                << ", format=" << static_cast<int>(copy.format())
                << ", hasAlpha=" << copy.hasAlphaChannel()
                << ", cornerAlpha="
                << (copy.isNull() ? -1 : copy.pixelColor(0, 0).alpha());

    if (size) {
        *size = copy.size();
    }

    return copy;
}

}}
