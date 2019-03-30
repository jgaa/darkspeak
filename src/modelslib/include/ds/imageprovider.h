#ifndef IMAGEPROVIDER_H
#define IMAGEPROVIDER_H

#include <functional>
#include <QQuickImageProvider>

namespace ds {
namespace models {

class ImageProvider : public QQuickImageProvider
{
public:
    using provider_t = std::function<QImage (const QString& id)>;

    ImageProvider(const QString& name, provider_t provider);

    const QString& getName() const noexcept { return name_; }

    // QQmlImageProviderBase interface
public:
    ImageType imageType() const override;

    // QQuickImageProvider interface
public:
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:

    provider_t provider_;
    QString defaultImage_ = ":/images/anonymous.svg";
    const QString name_;
};

}}



#endif // IMAGEPROVIDER_H
