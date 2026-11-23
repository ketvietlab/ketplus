#include "cloud/CloudControlClient.h"
#include "storage/LocalJournal.h"

namespace ketplus::daemon {
namespace {
const QString forwardOnlyAccount = QStringLiteral("events-forward-only");
}

bool CloudControlClient::eventsForwardOnly() {
    if (!eventsForwardOnly_)
        eventsForwardOnly_ = storedString(forwardOnlyAccount) == uploadScope();
    return *eventsForwardOnly_;
}

void CloudControlClient::followEventsForwardOnly(const bool forwardOnly) {
    if (forwardOnly == eventsForwardOnly())
        return;
    QString error;
    if (forwardOnly) {
        // Takes effect even if it cannot be remembered; a restart then waits for the next answer.
        eventsForwardOnly_ = true;
        if (!storeString(forwardOnlyAccount, uploadScope(), &error))
            lastError_ = error;
        return;
    }
    // Events journaled while forward-only already reached clients live; uploading resumes after
    // them. The journal itself is kept. On failure the next answer tries again.
    if (!advanceUploadCursorsToHead())
        return;
    if (!credentials_.remove(forwardOnlyAccount, &error)) {
        lastError_ = error;
        return;
    }
    eventsForwardOnly_ = false;
}

// Only the upload cursor moves: acknowledging the head for journal retention here would delete
// events the host still serves as pages. Retention follows the next acknowledged upload.
bool CloudControlClient::advanceUploadCursorsToHead() {
    for (const StoredSession& session : journal_->sessions()) {
        const qint64 cursor = uploadedCursor(session.id, session.generation);
        if (cursor < 0)
            return false;
        if (session.lastCursor <= cursor)
            continue;
        QString error;
        if (!storeString(
                QStringLiteral("uploaded-cursor-%1-%2").arg(session.id).arg(session.generation),
                QString::number(session.lastCursor), &error)) {
            lastError_ = error;
            return false;
        }
        knownUploadedCursors_.insert(
            QStringLiteral("%1:%2").arg(session.id).arg(session.generation), session.lastCursor);
    }
    return true;
}

} // namespace ketplus::daemon
