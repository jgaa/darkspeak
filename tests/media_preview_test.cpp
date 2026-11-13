#include <QtTest>
#include <QBuffer>
#include <QImageReader>
#include <QtEndian>
#include <QTemporaryDir>
#include <QFile>
#include <QElapsedTimer>
#include <QScopeGuard>
#include "ds/mediapreview.h"
#include "ds/ffmpeg.h"
#include "logfault/logfault.h"
#include <iostream>
#include <thread>
#ifdef DS_VIDEO_PROBE
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace ds::core;
namespace {
QByteArray encode(QImage image, const char* format = "PNG") {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, format)) return {};
    return bytes;
}
}
class MediaPreviewTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        logfault::LogManager::Instance().AddHandler(std::make_unique<logfault::StreamHandler>(std::clog, logfault::LogLevel::DEBUGGING));
    }
    void actual_ffmpeg_video_frame_data() {
        QTest::addColumn<QString>("fixture_path"); QTest::addColumn<QSize>("frame_size");
        QTest::newRow("square-pixels") << QStringLiteral(DS_VIDEO_FIXTURE) << QSize(320, 180);
        QTest::newRow("anamorphic") << QFileInfo(QStringLiteral(DS_VIDEO_FIXTURE)).absolutePath() + "/anamorphic.mkv" << QSize(320, 135);
    }
    void actual_ffmpeg_video_frame() {
        QFETCH(QString, fixture_path); QFETCH(QSize, frame_size);
        QVERIFY2(!media_preview::ffmpegExecutable().isEmpty(), "Install FFmpeg to run the video regression tests");
        QFile fixture(fixture_path); QVERIFY(fixture.open(QIODevice::ReadOnly));
        const auto jpeg = media_preview::generateVideo(fixture.readAll());
        QVERIFY(!jpeg.isEmpty()); QVERIFY(jpeg.size() <= media_preview::max_encoded_bytes);
        const auto frame = media_preview::decode(jpeg);
        QCOMPARE(frame.size(), frame_size);
        QVERIFY(frame.pixelColor(100, 100).blue() > 200); QVERIFY(frame.pixelColor(100, 100).red() < 30);
        QVERIFY(frame.textKeys().isEmpty());
        QVERIFY(media_preview::generateVideo("not a video").isEmpty());
        QVERIFY(media_preview::generateVideo("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100\nhttp://127.0.0.1/private\n").isEmpty());
    }
    void subprocess_isolation_and_limits() {
#ifdef DS_VIDEO_PROBE
        QTemporaryDir programs; QVERIFY(programs.isValid());
        const auto old_path = qgetenv("PATH");
        const auto path_set = qEnvironmentVariableIsSet("PATH");
        auto restore = qScopeGuard([&] {
            if (path_set) qputenv("PATH", old_path); else qunsetenv("PATH");
            qunsetenv("DARKSPEAK_VIDEO_PROBE_SECRET");
        });
        qputenv("PATH", programs.path().toUtf8());
        QVERIFY(media_preview::ffmpegExecutable().isEmpty());
        QVERIFY(media_preview::generateVideo("missing helper").isEmpty());
        QVERIFY(QFile::link(QStringLiteral(DS_VIDEO_PROBE), programs.filePath("ffmpeg")));
        qputenv("DARKSPEAK_VIDEO_PROBE_SECRET", "test-only-secret");
        QImage image(32, 16, QImage::Format_RGB32); image.fill(Qt::blue);
        const auto jpeg = encode(image, "JPEG"); QVERIFY(!jpeg.isEmpty());
        QVERIFY(!media_preview::generateVideo("probe\n" + jpeg).isEmpty());
        {
            const int original_stdin = ::fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
            ::close(STDIN_FILENO);
            auto restore_stdin = qScopeGuard([original_stdin] {
                if (original_stdin >= 0) { ::dup2(original_stdin, STDIN_FILENO); ::close(original_stdin); }
                else ::close(STDIN_FILENO);
            });
            QVERIFY(!media_preview::generateVideo("probe\n" + jpeg).isEmpty());
        }
        QVERIFY(media_preview::generateVideo("flood\n" + jpeg).isEmpty());
        QElapsedTimer deadline; deadline.start();
        QVERIFY(media_preview::generateVideo("wait\n" + jpeg).isEmpty());
        QVERIFY(deadline.elapsed() >= media_preview::video_deadline_ms);
        QVERIFY(deadline.elapsed() < media_preview::video_deadline_ms + 3000);
        std::stop_source stopping;
        std::jthread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); stopping.request_stop();
        });
        deadline.restart();
        QVERIFY(media_preview::generateVideo("wait\n" + jpeg, stopping.get_token()).isEmpty());
        QVERIFY(stopping.stop_requested()); QVERIFY(deadline.elapsed() < 2000);
#endif
    }
    void raster_formats_data() {
        QTest::addColumn<QByteArray>("format");
        for (const auto& format : {QByteArray("PNG"), QByteArray("JPEG"), QByteArray("BMP")})
            QTest::newRow(format.constData()) << format;
    }
    void raster_formats() {
        QFETCH(QByteArray, format);
        QImage source(1600, 900, QImage::Format_RGB32);
        source.fill(QColor(20, 80, 160));
        source.setText("Description", "private EXIF GPS source-path fixture");
        source.setDotsPerMeterX(12345); source.setDotsPerMeterY(9876);
        const auto bytes = media_preview::generate(encode(source, format.constData()));
        QVERIFY(!bytes.isEmpty());
        QVERIFY(bytes.size() <= media_preview::max_encoded_bytes);
        QVERIFY(bytes.startsWith(QByteArray::fromHex("ffd8")));
        QVERIFY(!bytes.contains("private EXIF"));
        const auto preview = media_preview::decode(bytes);
        QCOMPARE(preview.size(), QSize(320, 180));
        QVERIFY(preview.textKeys().isEmpty());
        QVERIFY(!preview.hasAlphaChannel());
        // JPEG stores integer dots per inch; converting back to integer DPM
        // may truncate by one. Verify the fixed presentation density in DPI.
        QCOMPARE(preview.logicalDpiX(), 96); QCOMPARE(preview.logicalDpiY(), 96);
    }
    void alpha_is_flattened_and_small_images_are_not_enlarged() {
        QImage source(30, 50, QImage::Format_ARGB32);
        source.fill(Qt::transparent);
        const auto preview = media_preview::decode(media_preview::generate(encode(source)));
        QCOMPARE(preview.size(), source.size());
        QCOMPARE(preview.pixelColor(0, 0), QColor(Qt::white));
    }
    void active_corrupt_and_oversized_sources_are_rejected() {
        QVERIFY(media_preview::generate({}).isEmpty());
        QVERIFY(media_preview::generate("not an image").isEmpty());
        QVERIFY(media_preview::generate("<svg xmlns='http://www.w3.org/2000/svg' width='20' height='20'><image href='https://invalid.test/private'/></svg>").isEmpty());
        QVERIFY(media_preview::generate(QByteArray(media_preview::max_source_bytes + 1, 'x')).isEmpty());
        // A tiny BMP with a hostile declared size fails before pixel allocation.
        QImage source(2, 2, QImage::Format_RGB32); source.fill(Qt::red);
        auto bmp = encode(source, "BMP"); QVERIFY(bmp.size() >= 26);
        qToLittleEndian<quint32>(100000, bmp.data() + 18);
        qToLittleEndian<quint32>(100000, bmp.data() + 22);
        QVERIFY(media_preview::generate(bmp).isEmpty());
        QImage too_many_pixels(4097, 4096, QImage::Format_RGB32); too_many_pixels.fill(Qt::black);
        QVERIFY(media_preview::generate(encode(too_many_pixels)).isEmpty());
    }
    void cached_format_is_strictly_jpeg_with_small_dimensions() {
        QImage image(320, 320, QImage::Format_RGB32); image.fill(Qt::blue);
        QVERIFY(media_preview::decode(encode(image)).isNull());
        QVERIFY(media_preview::decode(encode(image, "JPEG")).size() == image.size());
        QImage large(321, 1, QImage::Format_RGB32); large.fill(Qt::red);
        QVERIFY(media_preview::decode(encode(large, "JPEG")).isNull());
        QVERIFY(media_preview::decode(QByteArray(media_preview::max_encoded_bytes + 1, 'x')).isNull());
        QVERIFY(media_preview::decode("corrupt").isNull());
    }
};
QTEST_GUILESS_MAIN(MediaPreviewTest)
#include "media_preview_test.moc"
