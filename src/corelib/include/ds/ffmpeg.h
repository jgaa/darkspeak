#pragma once

#include <QFileInfo>
#include <QStandardPaths>
#include <QString>

namespace ds::core::media_preview {
inline QString ffmpegExecutable() {
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    const auto path = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    return path.isEmpty() ? QString{} : QFileInfo(path).canonicalFilePath();
#else
    // Before enabling Windows, Android or macOS builds, find and implement an
    // appropriate platform backend/deployment alternative to this Linux CLI.
    return {};
#endif
}
}
