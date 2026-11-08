#include "cloud/CloudControlClient.h"
#include "storage/LocalJournal.h"
#include <QJsonDocument>

namespace ketplus::daemon {
QString CloudControlClient::uploadScope() const {
    return QString::fromUtf8(QJsonDocument(QJsonArray{QStringLiteral("cloud-upload-v1"),
                                                      baseUrl_.toString(QUrl::FullyEncoded),
                                                      workspaceId_, machineId_, machineKeyVersion_})
                                 .toJson(QJsonDocument::Compact));
}
qint64 CloudControlClient::uploadedCursor(const QString& sessionId, qint64 generation) {
    const qint64 durable = journal_->cloudUploadCursor(uploadScope(), sessionId, generation);
    if (durable < 0) {
        lastError_ = journal_->errorString();
        return -1;
    }
    return qMax(durable,
                storedString(QStringLiteral("uploaded-cursor-%1-%2").arg(sessionId).arg(generation))
                    .toLongLong());
}

void CloudControlClient::uploadEvents() {
    if (machineId_.isEmpty() || daemonToken_.isEmpty())
        return;
    const QList<StoredSession> sessions = journal_->sessions();
    // Every upload used to re-register every session's cursor, one journal transaction each, on
    // the daemon's only thread. Only a session this process has not registered yet needs one.
    for (const StoredSession& session : sessions) {
        if (!registeredCloudCursors_.contains(
                QStringLiteral("%1:%2").arg(session.id).arg(session.generation)) &&
            !registerCloudCursor(session))
            return;
    }
    for (const StoredSession& session : sessions) {
        if (refusedSessions_.contains(session.id))
            continue;
        const qint64 cursor = uploadedCursor(session.id, session.generation);
        if (cursor < 0)
            return;
        if (cursor < session.lastCursor)
            uploadSessionEvents(session.id, session.generation, cursor);
    }
}

bool CloudControlClient::registerCloudCursors() {
    registeredCloudCursors_.clear();
    for (const StoredSession& session : journal_->sessions()) {
        if (!registerCloudCursor(session))
            return false;
    }
    return true;
}

bool CloudControlClient::registerCloudCursor(const StoredSession& session) {
    const qint64 cursor = uploadedCursor(session.id, session.generation);
    if (cursor < 0)
        return false;
    const QString account =
        QStringLiteral("uploaded-cursor-%1-%2").arg(session.id).arg(session.generation);
    if (cursor > storedString(account).toLongLong()) {
        QString error;
        if (!storeString(account, QString::number(cursor), &error)) {
            lastError_ = error;
            return false;
        }
    }
    if (!journal_->acknowledgeCursor(QStringLiteral("cloud-control-v2"), session.id,
                                     session.generation, cursor)) {
        lastError_ = journal_->errorString();
        return false;
    }
    registeredCloudCursors_.insert(QStringLiteral("%1:%2").arg(session.id).arg(session.generation));
    return true;
}

} // namespace ketplus::daemon
