#include "protocol/DaemonProtocol.h"
#include "runtime/SessionHost.h"

#include <QJsonDocument>
#include <optional>

namespace ketplus::daemon {
namespace {
QJsonObject error(const QString& code, const QString& message) {
    return {{"ok", false}, {"error_code", code}, {"error_message", message}};
}
} // namespace

QJsonObject SessionHost::listSessions(const QJsonObject& request) const {
    const auto payload = request.value("payload").toObject();
    const bool paged = payload.contains("deleted_sessions_page_version");
    const bool liveOnly = payload.value("session_catalog_live_only").toBool();
    if ((payload.contains("session_catalog_live_only") &&
         !payload.value("session_catalog_live_only").isBool()) ||
        (liveOnly && (!paged || payload.contains("deleted_sessions_after") ||
                      payload.contains("deleted_sessions_snapshot") ||
                      payload.contains("deleted_sessions_only"))))
        return error("invalid_session_catalog", "Invalid live-only catalog request.");
    if ((paged && payload.value("deleted_sessions_page_version").toInteger(-1) != 1) ||
        (payload.contains("deleted_sessions_only") &&
         !payload.value("deleted_sessions_only").isBool()) ||
        (payload.contains("deleted_sessions_after") &&
         (!paged || !payload.value("deleted_sessions_after").isObject() ||
          payload.value("deleted_sessions_after").toObject().isEmpty())))
        return error("invalid_tombstone_page", "Unsupported tombstone page or cursor.");
    std::optional<QString> snapshot;
    QJsonObject catalog{{"ok", true}};
    if (liveOnly) {
        // Discovery does not reconcile deletion history. Do not read either the
        // tombstone rows or their revision, and never label this a complete walk.
        catalog.insert("session_catalog_live_only", true);
    } else {
        snapshot = journal_->sessionTombstoneSnapshot();
        if (!snapshot)
            return error("journal_error", journal_->errorString());
        const auto supplied = payload.value("deleted_sessions_snapshot");
        if ((payload.contains("deleted_sessions_after") && !supplied.isString()) ||
            (!supplied.isUndefined() &&
             (!paged || !supplied.isString() || supplied.toString().isEmpty() ||
              supplied.toString().size() > 128)))
            return error("invalid_tombstone_page", "Invalid tombstone snapshot token.");
        if (!supplied.isUndefined() && supplied.toString() != *snapshot)
            return error("session_catalog_changed",
                         "The deletion catalog changed; restart the walk.");
        const auto page =
            journal_->sessionTombstones(payload.value("deleted_sessions_after").toObject(), !paged);
        if (!page)
            return error("journal_error", journal_->errorString());
        const auto afterRead = journal_->sessionTombstoneSnapshot();
        if (!afterRead)
            return error("journal_error", journal_->errorString());
        if (*snapshot != *afterRead)
            return error("session_catalog_changed",
                         "The deletion catalog changed; restart the walk.");
        if (!paged && !page->next.isEmpty())
            return error("session_catalog_upgrade_required",
                         "This deletion catalog requires a client with tombstone pagination.");
        catalog.insert("deleted_sessions", page->tombstones);
        if (paged) {
            catalog.insert("deleted_sessions_page_version", 1);
            catalog.insert("deleted_sessions_snapshot", *snapshot);
            catalog.insert("deleted_sessions_next", page->next);
        }
    }
    const auto finish = [&](const QJsonObject& catalog) {
        if (snapshot) {
            const auto finalSnapshot = journal_->sessionTombstoneSnapshot();
            if (!finalSnapshot)
                return error("journal_error", journal_->errorString());
            if (*snapshot != *finalSnapshot)
                return error("session_catalog_changed",
                             "The deletion catalog changed; restart the walk.");
        }
        const auto wireBytes = [&](const QJsonObject& value) {
            return QJsonDocument(protocol::response(request, value))
                .toJson(QJsonDocument::Compact)
                .size();
        };
        if (wireBytes(catalog) <= protocol::maximumFrameBytes)
            return catalog;
        auto withoutDeletions = catalog;
        withoutDeletions.insert("deleted_sessions", QJsonArray{});
        if (!paged && wireBytes(withoutDeletions) <= protocol::maximumFrameBytes)
            return error("session_catalog_upgrade_required",
                         "This deletion catalog requires a client with tombstone pagination.");
        return error("session_catalog_too_large",
                     "The live session catalog exceeds its byte limit.");
    };
    if (!liveOnly && paged &&
        (payload.contains("deleted_sessions_after") ||
         payload.value("deleted_sessions_only").toBool()))
        return finish(catalog); // A continuation never reloads the live session catalog.

    QJsonArray values;
    const QHash<QString, QString> numbers = journal_->conversationNumbers();
    const QHash<QString, QJsonObject> agentTurns = agentTurnSenders();
    for (const Session& session : sessions_) {
        const StoredSession& stored = session.stored;
        QJsonObject value{{QStringLiteral("session_id"), stored.id},
                          {QStringLiteral("session_generation"), stored.generation},
                          {QStringLiteral("worktree_path"), stored.worktreePath},
                          {QStringLiteral("context_revision"), stored.contextRevision},
                          {"configuration_revision", stored.configurationRevision},
                          {"effective", configurationOf(stored, {}).value("effective")},
                          {QStringLiteral("conversation_path"), journal_->conversationPath(stored)},
                          {QStringLiteral("adapter"), stored.adapter},
                          {QStringLiteral("title"), stored.title},
                          {QStringLiteral("state"), stored.state},
                          {QStringLiteral("current_turn_sequence"), stored.currentTurnSequence},
                          {QStringLiteral("last_event_cursor"), stored.lastCursor},
                          {QStringLiteral("last_seen_cursor"), stored.lastSeenCursor},
                          {QStringLiteral("last_turn_started_at"), stored.lastTurnStartedAt}};
        if (const QString number = numbers.value(stored.id); !number.isEmpty())
            value.insert(QStringLiteral("number"), number);
        if (!stored.archivedAt.isEmpty())
            value.insert(QStringLiteral("archived_at"), stored.archivedAt);
        if (const auto sender = agentTurns.constFind(stored.id); sender != agentTurns.cend())
            value.insert(QStringLiteral("agent_turn"), *sender);
        if (!session.backgroundTasks.isEmpty())
            value.insert(QStringLiteral("background_tasks"), session.backgroundTasks);
        values.append(value);
    }
    const QJsonObject live{
        {QStringLiteral("sessions"), values},
        {QStringLiteral("worktree_context_version"), 1},
        {QStringLiteral("transcript_window_version"), 1},
        {QStringLiteral("transcript_large_event_version"), 1},
        {QStringLiteral("transcript_large_event_max_bytes"), 32 * 1024 * 1024},
        {QStringLiteral("transcript_large_event_fragment_bytes"), 128 * 1024},
        {QStringLiteral("transcript_window_max_events"), 128},
        {QStringLiteral("transcript_window_max_bytes"), 512 * 1024},
        {QStringLiteral("transcript_window_max_turns"), LocalJournal::maximumTranscriptWindowTurns},
        {QStringLiteral("transcript_window_after_cursor"), true},
        {QStringLiteral("replay_page_max_events"), protocol::maximumReplayEvents},
        {QStringLiteral("replay_page_max_bytes"),
         static_cast<qint64>(protocol::maximumReplayPayloadBytes)}};
    for (auto it = live.begin(); it != live.end(); ++it)
        catalog.insert(it.key(), it.value());
    return finish(catalog);
}

} // namespace ketplus::daemon
