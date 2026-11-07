#pragma once

#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

class QIODevice;

namespace ketplus::daemon {

// Owner-only, content-addressed store for media returned inline by MCP tools (ADR 0008).
//
// Layout (daemon state directory, next to journal.sqlite3):
//   media/            0700
//   media/<sha256>.png|jpg   0600, original bytes
//   media/refs/<sha256(session id)>.refs   0600, one media hash per line
//
// The journal only ever stores {type:"image", media_ref:"sha256:<hex>", mime, bytes, width?,
// height?}. Desktop and daemon run as the same user; the desktop resolves refs by reading the
// same directory (see mcp_media::daemonMediaDirectory()). No bytes are served over IPC.
class McpMediaStore final {
  public:
    static constexpr int maximumImagesPerCall = 4;
    static constexpr qsizetype maximumImageBytes = 16 * 1024 * 1024;
    static constexpr qint64 defaultCapacityBytes = 512LL * 1024 * 1024;

    explicit McpMediaStore(QString directory, qint64 capacityBytes = defaultCapacityBytes);

    // <directory of journalPath>/media
    [[nodiscard]] static QString directoryForJournal(const QString& journalPath);
    [[nodiscard]] QString directory() const { return directory_; }

    // Returns item with every `result.content[]` image block replaced by a media reference and
    // every other media block (audio, invalid, oversized or beyond the per-call limit) replaced
    // by {type, omitted:true, reason}. Items without media blocks are returned unchanged.
    // imageView.path and imageGeneration.savedPath are copied before temporary provider or
    // browser files disappear. Their result.content uses the same references as MCP images.
    [[nodiscard]] QJsonObject externalizeItem(const QJsonObject& item, const QString& sessionId,
                                              const QString& worktree = {});

    // A forked session references the same media as its source, so each keeps it alive.
    void copySessionReferences(const QString& fromSessionId, const QString& toSessionId);
    // Stores an image attached to a message (from the phone or the desktop composer) so the
    // transcript can show it after the provider's temporary copy is gone. Returns the same
    // reference block as externalizeItem, or an empty object for anything but a PNG or JPEG
    // within maximumImageBytes, or when the store cannot be written.
    [[nodiscard]] QJsonObject storeImage(const QByteArray& bytes, const QString& mimeType,
                                         const QString& sessionId);
    // A bounded JPEG of a referenced image, for a phone that asked to see it.
    [[nodiscard]] QJsonObject preview(const QString& sessionId, const QString& reference,
                                      int maximumSide) const;
    // The same bounded JPEG (no larger than maximumSide, at most 192 KB) of any image source:
    // {ok, mime, width, height, data} with base64url data, or {ok:false, error_code}.
    [[nodiscard]] static QJsonObject encodePreview(QIODevice* source, int maximumSide);
    // Drops the session's references and deletes media no other session references.
    void releaseSession(const QString& sessionId);
    // Releases every reference file whose session is not in sessionIds (sessions removed while
    // the daemon was not running, or by a journal path that bypassed releaseSession).
    void retainSessions(const QSet<QString>& sessionIds);
    // Evicts oldest unreferenced media first, then (with a warning) oldest referenced media,
    // until the store fits its capacity.
    void enforceCapacity();

  private:
    [[nodiscard]] QString refsPath(const QString& sessionId) const;
    // Writes (or refreshes) the content-addressed file; empty on failure.
    [[nodiscard]] QJsonObject write(const QByteArray& bytes, const QString& extension) const;
    // Atomically records ownership before publishing references; false preserves
    // unsupported metadata. Capacity enforcement follows a successful commit.
    [[nodiscard]] bool reference(const QString& sessionId, const QStringList& hashes);

    QString directory_;
    qint64 capacityBytes_;
};

} // namespace ketplus::daemon
