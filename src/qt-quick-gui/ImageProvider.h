
#include "darkspeak/Api.h"
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

    std::shared_ptr<QImage> get(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = images_.find(key);
        if (it != images_.end()) {
            return it->second;
        }
        return {};
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

    void rename(const std::string& oldKey,
                const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = images_.find(oldKey);
        if (it != images_.end()) {
            images_[key] = it->second;
            images_.erase(it);
        }
    }

    void save(const std::string& key, const darkspeak::path_t& path);

private:
    std::map<std::string, std::shared_ptr<QImage>> images_;
    QImage missing_;
    mutable std::mutex mutex_;
};

