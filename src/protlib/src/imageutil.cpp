
#include "ds/errors.h"
#include "include/ds/imageutil.h"

#include "logfault/logfault.h"

namespace ds {
namespace prot {

using namespace core;

QJsonObject toJson(const QImage &image)
{
    if (image.isNull()) {
        // Remove avatar
        return QJsonObject {
            {"height", 0},
            {"width", 0}
        };
    }

    const auto bytes = image.width() * image.height();

    QByteArray r(bytes, 0), g(bytes, 0), b(bytes, 0);

    int ix = 0;
    for(int y = 0; y < image.height(); ++y) {
        for(int x = 0; x < image.width(); ++x, ++ix) {
            const auto pixel = image.pixel(x, y);

            r[ix] = static_cast<char>(static_cast<uint8_t>(qRed(pixel)));
            g[ix] = static_cast<char>(static_cast<uint8_t>(qGreen(pixel)));
            b[ix] = static_cast<char>(static_cast<uint8_t>(qBlue(pixel)));
        }
    }

    return QJsonObject {
        {"height", image.height()},
        {"width", image.width()},
        { "r", QString{r.toBase64()}},
        { "g", QString{g.toBase64()}},
        { "b", QString{b.toBase64()}},
    };
}

QImage toQimage(const QJsonObject &object)
{
    const auto height = object.value("height").toInt();
    const auto width = object.value("width").toInt();

    if ((height == 0) && (width == 0)) {
        // Remove avatar
        return {};
    }

    if ((height <= 0) || (width <= 0) || (height > 128) || (width > 128)) {
        LFLOG_WARN << "Invalid image size: height=" << height
                   << ", width=" << width;
        throw Error("Invalid image size from json");
    }

    const auto rd = QByteArray::fromBase64(object.value("r").toString().toUtf8());
    const auto gd = QByteArray::fromBase64(object.value("g").toString().toUtf8());
    const auto bd = QByteArray::fromBase64(object.value("b").toString().toUtf8());

    const auto bytes = width * height;

    if ((rd.size() != bytes) || (gd.size() != bytes) || (bd.size() != bytes)) {
        throw Error("Invalid rgb data from json");
    }

    auto img = QImage{width, height, QImage::Format_RGB32};

    int ix = 0;
    for(int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x, ++ix) {

            const uint8_t r = static_cast<uint8_t>(rd[ix]);
            const uint8_t g = static_cast<uint8_t>(gd[ix]);
            const uint8_t b = static_cast<uint8_t>(bd[ix]);

            img.setPixel(x, y, qRgb(r, g, b));
        }
    }
    return img;
}

}} // namespaces
