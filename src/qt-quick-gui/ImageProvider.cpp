
#include <boost/filesystem.hpp>
#include "log/WarLog.h"
#include "ImageProvider.h"
#include <qt5/QtCore/QString>

using namespace war;
using namespace darkspeak;

ImageProvider::ImageProvider()
: QQuickImageProvider(QQuickImageProvider::Pixmap)
{
    missing_.load(":/images/Missing.svg");
}

QImage ImageProvider::requestImage(const QString &id, QSize *size,
                                   const QSize &requestedSize) {

    LOG_DEBUG << "Requested: " << log::Esc(id.toStdString());

    QImage img;
    std::lock_guard<std::mutex> lock(mutex_);

    auto key = id.toStdString();
    const auto pos = key.find_last_of(':');
    if (pos != key.npos) {
        key = key.substr(0, pos);
    }

    auto it = images_.find(key);
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

void ImageProvider::save(const std::string& key,
                         const path_t& path) {

    if (!boost::filesystem::is_directory(path.parent_path())) {
        LOG_DEBUG_FN << "Creating directory "
            << log::Esc(path.parent_path().string());
        boost::filesystem::create_directories(path.parent_path());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = images_.find(key);
    if (it != images_.end()) {
        it->second->save(path.c_str());
    }
}
