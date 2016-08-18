
#include "ImageProvider.h"
#include <qt5/QtCore/QString>


ImageProvider::ImageProvider()
: QQuickImageProvider(QQuickImageProvider::Pixmap)
{
    missing_.load(":/images/Missing.svg");
}

QImage ImageProvider::requestImage(const QString &id, QSize *size,
                                   const QSize &requestedSize) {

    QImage img;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = images_.find(id.toStdString());
    if (it != images_.end()) {
        if (requestedSize.isValid()) {
            img = it->second->scaled(requestedSize,
                                     Qt::KeepAspectRatioByExpanding,
                                     Qt::FastTransformation);
        } else {
            img = *it->second;
        }
    } else {
        const QSize use_size = requestedSize.isValid() ? requestedSize : QSize(64, 64);
        img = missing_.scaled(use_size);
    }

    if (size) {
        *size = img.size();
    }

    return img;
}

