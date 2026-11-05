#pragma once

#include <QString>

class QUrl;

namespace ketplus {

// Only transcript activation uses this policy; explicit editor/diff actions are unchanged.
bool isTranscriptTextFile(const QString& path);

// How a transcript link to an existing local path opens. Model output can name any path, so
// only viewer types (images, PDF, audio, video) reach the platform opener; folders, bundles,
// executables and every other type are revealed instead of launched.
enum class TranscriptFileAction { Edit, View, Reveal, Missing };
[[nodiscard]] TranscriptFileAction transcriptFileAction(const QString& path);

// What a transcript link with a scheme other than a local file may do.
enum class TranscriptUrlAction { Open, Confirm, Refuse };
[[nodiscard]] TranscriptUrlAction transcriptUrlAction(const QUrl& url);

// A UNC path or a file:// URL with a host. Even checking that it exists contacts that host
// (and on Windows sends the user's credentials), so it is refused before any file access.
[[nodiscard]] bool isRemoteFileLink(const QString& link);

} // namespace ketplus
