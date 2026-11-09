#include "runtime/SessionHost.h"
#include "workspace/ReadOnlyWorkspaceFiles.h"

#include <QBuffer>
#include <QPointer>
#include <QThreadPool>

namespace ketplus::daemon {
namespace {
// A phone opened an image from the file tree: a preview of the file as it is on disk now.
QJsonObject worktreeImagePreview(const QString& worktree, const QString& path) {
    if (path.isEmpty() || worktree.isEmpty())
        return {{"ok", false}, {"error_code", "invalid_path"}};
    QByteArray bytes;
    const QString error = ReadOnlyWorkspaceFiles({.rootPath = worktree}).readImage(path, &bytes);
    if (!error.isEmpty())
        return {{"ok", false}, {"error_code", error}};
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QJsonObject result = McpMediaStore::encodePreview(&buffer, 1280);
    if (result.value("ok").toBool())
        result.insert("path", path);
    return result;
}
} // namespace

QJsonObject SessionHost::ephemeralRead(const QJsonObject& request) {
    Session* session = nullptr;
    const auto failure = checkedSession(request, &session);
    if (!failure.isEmpty())
        return failure;
    const auto payload = request.value("payload").toObject();
    if (request.value("message_type") == "session.output.read") {
        const QString itemId = payload.value("item_id").toString();
        const qint64 turn = payload.value("turn_sequence").toInteger(-1);
        if (itemId.isEmpty() || itemId.size() > 256 || turn < 1 ||
            turn > session->stored.currentTurnSequence)
            return {{"ok", false}, {"error_code", "invalid_output_selector"}};
        return journal_->commandOutput(session->stored.id, session->stored.generation, turn,
                                       itemId);
    }
    if (payload.contains("path"))
        return worktreeImagePreview(session->stored.worktreePath, payload.value("path").toString());
    if (!journal_->containsMediaReference(session->stored.id, session->stored.generation,
                                          payload.value("media_ref").toString()))
        return {{"ok", false}, {"error_code", "media_unavailable"}};
    return mediaStore_.preview(session->stored.id, payload.value("media_ref").toString(),
                               payload.value("thumbnail").toBool() ? 160 : 1280);
}

void SessionHost::mediaReadDeferred(const QJsonObject& request, CommandCallback done) {
    Session* session = nullptr;
    if (const auto failure = checkedSession(request, &session); !failure.isEmpty()) {
        done(failure);
        return;
    }
    const auto payload = request.value("payload").toObject();
    if (payload.contains("path")) {
        // Reading and scaling a worktree image is as slow as a stored screenshot.
        startMediaRead(
            [worktree = session->stored.worktreePath, path = payload.value("path").toString()] {
                return worktreeImagePreview(worktree, path);
            },
            std::move(done));
        return;
    }
    const QString reference = payload.value("media_ref").toString();
    if (!journal_->containsMediaReference(session->stored.id, session->stored.generation,
                                          reference)) {
        done({{"ok", false}, {"error_code", "media_unavailable"}});
        return;
    }
    // Decoding and scaling a screenshot holds the main thread long enough to stall events and
    // other commands. The store only reads files here, so the preview is made on the pool.
    startMediaRead([store = mediaStore_, sessionId = session->stored.id, reference,
                    side = payload.value("thumbnail").toBool()
                               ? 160
                               : 1280] { return store.preview(sessionId, reference, side); },
                   std::move(done));
}

void SessionHost::startMediaRead(std::function<QJsonObject()> work, CommandCallback done) {
    // Beyond the queue the caller gets busy at once; waiting decodes would only hold more
    // memory.
    if (mediaReadsInFlight_ >= maximumMediaReads) {
        done({{"ok", false}, {"error_code", "busy"}});
        return;
    }
    ++mediaReadsInFlight_;
    mediaReads_.start([guard = QPointer<SessionHost>(this), work = std::move(work),
                       done = std::move(done)]() mutable {
        QJsonObject result = work();
        QMetaObject::invokeMethod(
            guard,
            [guard, done = std::move(done), result = std::move(result)] {
                --guard->mediaReadsInFlight_;
                done(result);
            },
            Qt::QueuedConnection);
    });
}
} // namespace ketplus::daemon
