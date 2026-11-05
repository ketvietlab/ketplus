#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVariantList>

namespace ketplus::mcp_media {

inline constexpr int maximumImagesPerCall = 4;
inline constexpr qsizetype maximumImageBytes = 16 * 1024 * 1024;
// Gallery source for a daemon media_ref whose file is gone; it never loads, so the activity
// gallery renders its "Screenshot unavailable" placeholder in that slot.
inline constexpr auto unavailableSourcePrefix = "ketplus-media-unavailable:";

// Owner-only cache directory for images returned inline by MCP tools.
[[nodiscard]] QString directory();

// Owner-only media store of the local daemon: <dir of DaemonClient::defaultStatePath()>/media.
// Daemon-backed transcripts carry media_ref blocks into this directory.
[[nodiscard]] QString daemonMediaDirectory();

// Decodes bounded PNG/JPEG `image` content blocks from an MCP tool result, stores each once
// under directory() by content hash and returns file URLs for the transcript image gallery.
// Daemon `media_ref` blocks resolve to existing files in mediaDirectory (no copy); a ref that
// cannot be resolved yields an unavailableSourcePrefix placeholder. Oversized or malformed
// inline blocks and other media types are skipped.
[[nodiscard]] QVariantList
materializeImages(const QJsonObject& result, const QString& targetDirectory = directory(),
                  const QString& mediaDirectory = daemonMediaDirectory());

// File URLs for the images a message was sent with: the `images` array of a daemon
// user.message, media_ref blocks in mediaDirectory. Images the store no longer has are left out.
[[nodiscard]] QVariantList messageImages(const QJsonArray& blocks,
                                         const QString& mediaDirectory = daemonMediaDirectory());

// Short, sanitized one-line summary of scalar tool arguments (ids and secrets excluded).
[[nodiscard]] QString argumentSummary(const QJsonValue& arguments);

} // namespace ketplus::mcp_media
