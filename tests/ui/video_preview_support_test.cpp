#include <QtTest>
#include <QTemporaryDir>
#include "video_preview_support.h"

class VideoPreviewSupportTest final : public QObject {
    Q_OBJECT
private slots:
    void commands_data() {
        QTest::addColumn<QByteArray>("release"); QTest::addColumn<QString>("command");
        QTest::newRow("ubuntu") << QByteArray("ID=ubuntu\nID_LIKE=debian\n") << QString("sudo apt install ffmpeg");
        QTest::newRow("mint") << QByteArray("ID=linuxmint\nID_LIKE=\"ubuntu debian\"\n") << QString("sudo apt install ffmpeg");
        QTest::newRow("arch") << QByteArray("ID=arch\n") << QString("sudo pacman -S ffmpeg");
        QTest::newRow("manjaro") << QByteArray("ID=manjaro\nID_LIKE='arch'\n") << QString("sudo pacman -S ffmpeg");
        QTest::newRow("fedora") << QByteArray("ID=fedora\n") << QString("sudo dnf install ffmpeg-free");
        QTest::newRow("rhel-repositories-vary") << QByteArray("ID=rhel\nID_LIKE=fedora\n") << QString();
        QTest::newRow("alpine") << QByteArray("ID=alpine\n") << QString("sudo apk add ffmpeg");
        QTest::newRow("unknown") << QByteArray("ID=unknown\n") << QString();
        QTest::newRow("shell-not-interpreted") << QByteArray("ID=$(touch /tmp/not-executed)\n") << QString();
        QTest::newRow("oversize") << QByteArray(16385, 'x') << QString();
    }
    void commands() {
        QFETCH(QByteArray, release); QFETCH(QString, command);
        QCOMPARE(VideoPreviewSupport::installationCommand(release), command);
    }
    void opt_out_persists_and_regular_dismissal_does_not() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("profile.ini");
        QSettings settings(path, QSettings::IniFormat);
        const VideoPreviewSupport::Environment missing{true, false, "ID=ubuntu"};
        VideoPreviewSupport first(settings, missing); QVERIFY(first.warningRequired());
        first.dismiss(false); QVERIFY(!first.warningRequired());
        VideoPreviewSupport next(settings, missing); QVERIFY(next.warningRequired());
        next.dismiss(true); QVERIFY(!next.warningRequired());
        QSettings reopened(path, QSettings::IniFormat);
        VideoPreviewSupport after_restart(reopened, missing); QVERIFY(!after_restart.warningRequired());
        QSettings other(directory.filePath("another-profile.ini"), QSettings::IniFormat);
        VideoPreviewSupport independent(other, missing); QVERIFY(independent.warningRequired());
    }
    void available_flatpak_and_other_platforms_do_not_warn() {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("profile.ini"), QSettings::IniFormat);
        VideoPreviewSupport available(settings, {true, true, {}}); QVERIFY(!available.warningRequired());
        VideoPreviewSupport flatpak_or_other(settings, {false, false, {}}); QVERIFY(!flatpak_or_other.warningRequired());
    }
};
QTEST_GUILESS_MAIN(VideoPreviewSupportTest)
#include "video_preview_support_test.moc"
