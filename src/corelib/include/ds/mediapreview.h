#pragma once

#include <QByteArray>
#include <QImage>
#include <stop_token>

namespace ds::core::media_preview {

// Version 1 is a metadata-free, opaque JPEG. These limits are independent of
// file-transfer limits; being willing to store a file does not authorize parsing it.
inline constexpr int version = 1;
inline constexpr int edge = 320;
inline constexpr qsizetype max_encoded_bytes = 48 * 1024;
inline constexpr qsizetype max_source_bytes = 32 * 1024 * 1024;
inline constexpr qsizetype max_video_source_bytes = 128 * 1024 * 1024;
inline constexpr int video_deadline_ms = 10000;
inline constexpr qint64 max_source_pixels = 16 * 1024 * 1024;
inline constexpr int max_source_dimension = 32768;
inline constexpr int max_cached_previews = 512;

inline bool isVideoCandidate(const QString& mime_type) {
    // Content-only sniffing cannot distinguish Matroska audio and video tracks.
    // Keep its honest container MIME; the bounded decoder must find a video
    // stream before a preview exists. Never infer a decoder from a filename.
    return mime_type.startsWith("video/") || mime_type == "application/x-matroska";
}

// Call generation on a bounded worker, never on the GUI thread. Unsupported,
// active, oversized, corrupt, and undecodable inputs return an empty result.
QByteArray generate(QByteArray source);
QByteArray generateVideo(QByteArray source, std::stop_token stop = {});
QImage decode(QByteArray jpeg);

} // namespace ds::core::media_preview
