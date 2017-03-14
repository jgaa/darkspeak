
#include <boost/filesystem.hpp>
#include "log/WarLog.h"
#include "ImageProvider.h"
#include <qt5/QtCore/QString>

using namespace std;
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
        LOG_DEBUG << "Could not find image " << log::Esc(id.toStdString())
            << ". Using default (missing).";
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
    } else {
        QString qpath = path.c_str();
        auto img = make_shared<QImage>(qpath);
        if (img && !img->isNull()) {
            images_[key] = move(img);
        } else {
            LOG_DEBUG_FN << "Failed to find image " << log::Esc(key);
        }
    }
}

void ImageProvider::add(const std::string key, std::shared_ptr<QImage> img) {
    std::lock_guard<std::mutex> lock(mutex_);
        WAR_ASSERT(img != nullptr);
        images_[key] = std::move(img);
}

 void ImageProvider::remove(const std::string key) {
    std::lock_guard<std::mutex> lock(mutex_);
    images_.erase(key);
}

bool ImageProvider::haveImage(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return images_.find(key) != images_.end();
}

void ImageProvider::rename(const std::string& oldKey,
            const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = images_.find(oldKey);
    if (it != images_.end()) {
        LOG_DEBUG_FN << "Renaming image " << log::Esc(oldKey) << " to "
            << log::Esc(key);
        images_[key] = it->second;
        images_.erase(it);
    } else {
        LOG_DEBUG_FN << "Failed to find image " << log::Esc(oldKey);
        log_all();
    }
}

void ImageProvider::log_all() {
    for(const auto& it : images_) {
        LOG_DEBUG << "Images: ";
        LOG_DEBUG << "  Image key=" << log::Esc(it.first);
    }
}
