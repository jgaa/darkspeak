#include "ds/mediapreview.h"

#include <QBuffer>
#include <QColorSpace>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>

namespace ds::core::media_preview {
namespace {
bool validSize(QSize size, qint64 pixels, int dimension) {
    return size.width() > 0 && size.height() > 0
        && size.width() <= dimension && size.height() <= dimension
        && qint64(size.width()) * size.height() <= pixels;
}
}

QByteArray generate(QByteArray source) {
    if (source.isEmpty() || source.size() > max_source_bytes) return {};
    QBuffer input(&source);
    if (!input.open(QIODevice::ReadOnly)) return {};
    QImageReader reader(&input);
    reader.setDecideFormatFromContent(true);
    // Deliberately raster only. In particular, an SVG must never gain access
    // to external resources through the thumbnail-generation path.
    const auto format = reader.format().toLower();
    const QList<QByteArray> raster_formats{
        "jpeg", "jpg", "png", "webp", "gif", "bmp", "ico", "cur",
        "tiff", "tif", "tga", "pbm", "pgm", "ppm", "xbm", "xpm",
        "avif", "heif", "heic", "jp2"};
    if (!raster_formats.contains(format)
        || !validSize(reader.size(), max_source_pixels, max_source_dimension)) return {};
    reader.setAutoTransform(true);
    if (reader.size().width() > edge || reader.size().height() > edge)
        reader.setScaledSize(reader.size().scaled(edge, edge, Qt::KeepAspectRatio));
    auto image = reader.read(); // First frame only; never animate a preview.
    if (image.isNull() || !validSize(image.size(), max_source_pixels, max_source_dimension)) return {};
    if (image.width() > edge || image.height() > edge)
        image = image.scaled(edge, edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (image.colorSpace().isValid()) image.convertToColorSpace(QColorSpace::SRgb);
    // Copy pixels to a fresh image: no EXIF, comments, text keys, source paths,
    // ICC blobs, GPS, animation, or other source metadata survives re-encoding.
    QImage clean(image.size(), QImage::Format_RGB32);
    // Fixed density: neither source metadata nor desktop/screen DPI is encoded.
    clean.setDotsPerMeterX(3780); clean.setDotsPerMeterY(3780);
    clean.setDevicePixelRatio(1.0);
    clean.fill(Qt::white);
    {
        QPainter painter(&clean);
        painter.drawImage(0, 0, image);
    }
    QByteArray bytes;
    QBuffer output(&bytes);
    if (!output.open(QIODevice::WriteOnly)) return {};
    QImageWriter writer(&output, "JPEG");
    writer.setQuality(70);
    writer.setOptimizedWrite(true);
    if (!writer.write(clean) || bytes.size() > max_encoded_bytes) return {};
    return bytes;
}

QImage decode(QByteArray jpeg) {
    if (jpeg.size() < 3 || jpeg.size() > max_encoded_bytes || !jpeg.startsWith(QByteArray::fromHex("ffd8ff"))) return {};
    QBuffer input(&jpeg);
    if (!input.open(QIODevice::ReadOnly)) return {};
    QImageReader reader(&input, "JPEG");
    reader.setAutoDetectImageFormat(false);
    if (!validSize(reader.size(), qint64(edge) * edge, edge)) return {};
    auto image = reader.read();
    if (image.isNull() || !validSize(image.size(), qint64(edge) * edge, edge)) return {};
    return image;
}
} // namespace ds::core::media_preview
