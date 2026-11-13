#include "video_preview_support.h"
#include "ds/ffmpeg.h"
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <utility>

VideoPreviewSupport::VideoPreviewSupport(QSettings& settings, QObject* parent)
    : VideoPreviewSupport(settings, detect(), parent) {}

VideoPreviewSupport::VideoPreviewSupport(QSettings& settings, Environment environment, QObject* parent)
    : QObject(parent), settings_(settings), missing_native_(environment.native_linux && !environment.ffmpeg_available),
      install_command_(installationCommand(std::move(environment.os_release))) {}

VideoPreviewSupport::Environment VideoPreviewSupport::detect() {
    Environment result;
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    result.ffmpeg_available = !ds::core::media_preview::ffmpegExecutable().isEmpty();
#ifndef DARKSPEAK_FLATPAK_BUILD
    result.native_linux = !QFileInfo::exists(QStringLiteral("/.flatpak-info"))
        && !qEnvironmentVariableIsSet("FLATPAK_ID");
#endif
    for (const auto* path : {"/etc/os-release", "/usr/lib/os-release"}) {
        QFile file(QString::fromLatin1(path));
        if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly)) continue;
        result.os_release = file.read(16385);
        break;
    }
#else
    // Windows, Android and macOS will need platform-specific dependency advice
    // and a supported video-thumbnail backend before those builds are enabled.
#endif
    return result;
}

QString VideoPreviewSupport::installationCommand(QByteArray os_release) {
    if (os_release.size() > 16384) return {};
    QHash<QByteArray, QByteArray> values;
    for (const auto& line : os_release.split('\n')) {
        const auto split = line.indexOf('=');
        if (split < 0) continue;
        const auto name = line.first(split).trimmed();
        if (name != "ID" && name != "ID_LIKE") continue;
        auto value = line.sliced(split + 1).trimmed();
        if (value.size() > 256) continue;
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"')
            || (value.front() == '\'' && value.back() == '\''))) value = value.mid(1, value.size() - 2);
        values.insert(name, value);
    }
    // Select only fixed commands. Never interpolate release metadata or invoke
    // a shell; the dialog provides instructions, not an installation action.
    const auto id = values.value("ID");
    if (id == "fedora") return QStringLiteral("sudo dnf install ffmpeg-free");
    if (id == "alpine") return QStringLiteral("sudo apk add ffmpeg");
    static const QRegularExpression safe(QStringLiteral("^[a-z0-9._-]{1,64}$"));
    auto families = values.value("ID_LIKE").split(' ');
    families.prepend(id);
    for (const auto& family : families) {
        if (!safe.match(QString::fromLatin1(family)).hasMatch()) continue;
        if (family == "debian" || family == "ubuntu") return QStringLiteral("sudo apt install ffmpeg");
        if (family == "arch") return QStringLiteral("sudo pacman -S ffmpeg");
    }
    return {}; // Repository setup varies on other distributions; do not guess.
}

bool VideoPreviewSupport::warningRequired() const {
    return missing_native_ && !dismissed_
        && !settings_.value(QStringLiteral("mediaPreview/dontShowFfmpegWarning"), false).toBool();
}

void VideoPreviewSupport::dismiss(bool dont_show_again) {
    if (dont_show_again) {
        settings_.setValue(QStringLiteral("mediaPreview/dontShowFfmpegWarning"), true);
        settings_.sync();
    }
    dismissed_ = true;
    emit changed();
}
