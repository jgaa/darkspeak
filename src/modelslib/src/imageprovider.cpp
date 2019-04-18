
#include <QQmlEngine>

#include "include/ds/imageprovider.h"

#include "logfault/logfault.h"

using namespace std;

namespace ds {
namespace models {

ImageProvider::ImageProvider(QString name,
                             ImageProvider::provider_t provider)
    : QQuickImageProvider(QQuickImageProvider::Pixmap)
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

    LFLOG_TRACE << "Requesting image (" << getName() << ") " << key;

    auto image = provider_(key);
    if (image.isNull()) {
        image.load(defaultImage_);
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

    if (size) {
        *size = copy.size();
    }

    return copy;
}

}}

