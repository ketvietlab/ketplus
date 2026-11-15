#include "runtime/SessionHost.h"

#include "storage/LocalJournal.h"

#include <QDateTime>

#include <algorithm>
#include <utility>

namespace ketplus::daemon {
namespace {

QJsonObject ok(QJsonObject values = {}) {
    values.insert(QStringLiteral("ok"), true);
    return values;
}

QJsonObject error(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

} // namespace

QJsonObject SessionHost::setSessionArchived(const QJsonObject& request) {
    Session* session = nullptr;
    if (const QJsonObject failure = checkedSession(request, &session); !failure.isEmpty())
        return failure;
    const QJsonValue archived =
        request.value(QStringLiteral("payload")).toObject().value(QStringLiteral("archived"));
    if (!archived.isBool())
        return error(QStringLiteral("invalid_payload"),
                     QStringLiteral("`archived` must be true or false."));
    // ADR 0114: a member leaves its team before it is archived.
    if (archived.toBool())
        if (const auto refused = teams_->archiveRefusal(session->stored.id))
            return error(refused->code, refused->message);
    // Archiving again keeps the first time, so retention counts from when it was hidden.
    QString archivedAt = session->stored.archivedAt;
    if (!archived.toBool())
        archivedAt.clear();
    else if (archivedAt.isEmpty())
        archivedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    if (archivedAt != session->stored.archivedAt) {
        if (!journal_->setSessionArchived(session->stored.id, session->stored.generation,
                                          archivedAt))
            return error(QStringLiteral("journal_error"), journal_->errorString());
        session->stored.archivedAt = archivedAt;
        if (!archivedAt.isEmpty())
            disableAgentSchedules(session->stored.id);
    }
    QJsonObject result{{QStringLiteral("accepted"), true},
                       {QStringLiteral("session_id"), session->stored.id},
                       {QStringLiteral("session_generation"), session->stored.generation},
                       {QStringLiteral("archived"), !archivedAt.isEmpty()}};
    if (!archivedAt.isEmpty())
        result.insert(QStringLiteral("archived_at"), archivedAt);
    return ok(result);
}

bool SessionHost::expireArchivedSessions(const QDateTime& now) {
    if (!now.isValid())
        return false;
    // Unreadable options keep everything: expiry deletes history and cannot be undone.
    const auto options = journal_->storageOptions();
    if (!options || options->archivedHistoryDays <= 0)
        return false;
    const QDateTime cutoff = now.toUTC().addDays(-options->archivedHistoryDays);
    QList<std::pair<QDateTime, std::pair<QString, qint64>>> expired;
    for (const Session& session : std::as_const(sessions_)) {
        if (session.stored.archivedAt.isEmpty())
            continue;
        const QDateTime archivedAt =
            QDateTime::fromString(session.stored.archivedAt, Qt::ISODateWithMs);
        // An unreadable time is never treated as old.
        if (archivedAt.isValid() && archivedAt < cutoff)
            expired.append({archivedAt, {session.stored.id, session.stored.generation}});
    }
    std::sort(expired.begin(), expired.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    // Oldest first; a conversation that is busy (a running turn or background task) is skipped
    // by the delete guards and tried again on a later pass.
    for (const auto& [archivedAt, identity] : std::as_const(expired)) {
        const QString operationId =
            QStringLiteral("archive-retention:%1:%2").arg(identity.first).arg(identity.second);
        const QJsonObject deleted =
            deleteSession({{QStringLiteral("message_type"), QStringLiteral("session.delete")},
                           {QStringLiteral("message_id"), operationId},
                           {QStringLiteral("session_id"), identity.first},
                           {QStringLiteral("session_generation"), identity.second},
                           {QStringLiteral("payload"),
                            QJsonObject{{QStringLiteral("delete_operation_id"), operationId}}}});
        if (deleted.value(QStringLiteral("ok")).toBool()) {
            qInfo("journal: deleted conversation %s, archived %s", qPrintable(identity.first),
                  qPrintable(archivedAt.toString(Qt::ISODate)));
            return true;
        }
    }
    return false;
}

} // namespace ketplus::daemon
