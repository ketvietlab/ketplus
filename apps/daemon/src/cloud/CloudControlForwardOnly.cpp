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
    eventsForwardOnly_ = false;
}

// Only the upload cursor moves: acknowledging the head for journal retention here would delete
// events the host still serves as pages. Retention follows the next acknowledged upload. Every
// session's cursor and the forward-only record change in one credential commit.
bool CloudControlClient::advanceUploadCursorsToHead() {
    const QString scope = uploadScope();
    const QList<StoredSession> sessions = journal_->sessions();
    QHash<QString, qint64> advanced;
    QString error;
    const bool committed = credentials_.batch(
        [&](CredentialStore& view, QString* failure) {
            for (const StoredSession& session : sessions) {
                const qint64 durable =
                    journal_->cloudUploadCursor(scope, session.id, session.generation);
                if (durable < 0) {
                    *failure = journal_->errorString();
                    return false;
                }
                const QString account =
                    QStringLiteral("uploaded-cursor-%1-%2").arg(session.id).arg(session.generation);
                const qint64 cursor = qMax(durable, view.read(account).toLongLong());
                if (session.lastCursor <= cursor)
                    continue;
                if (!view.write(account, QByteArray::number(session.lastCursor), failure))
                    return false;
                advanced.insert(QStringLiteral("%1:%2").arg(session.id).arg(session.generation),
                                session.lastCursor);
            }
            return view.remove(forwardOnlyAccount, failure);
        },
        &error);
    if (!committed) {
        lastError_ = error;
        return false;
    }
    for (auto entry = advanced.cbegin(); entry != advanced.cend(); ++entry)
        knownUploadedCursors_.insert(entry.key(), entry.value());
    return true;
}

} // namespace ketplus::daemon
