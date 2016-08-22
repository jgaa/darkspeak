
#include "war_error_handling.h"
#include <map>
#include <memory>
#include <mutex>

#include <QQuickImageProvider>


class ImageProvider : public QQuickImageProvider
{
public:
    ImageProvider();

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

    ImageType imageType() const override {
        return QQmlImageProviderBase::Image;
    }

    void add(const std::string key, std::shared_ptr<QImage> img) {
        std::lock_guard<std::mutex> lock(mutex_);
        WAR_ASSERT(img != nullptr);
        images_[key] = std::move(img);
    }

    void remove(const std::string key) {
        std::lock_guard<std::mutex> lock(mutex_);
        images_.erase(key);
    }

    bool haveImage(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return images_.find(key) != images_.end();
    }

private:
    std::map<std::string, std::shared_ptr<QImage>> images_;
    QImage missing_;
    mutable std::mutex mutex_;
};

