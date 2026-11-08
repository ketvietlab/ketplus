#include "protocol/DaemonProtocol.h"
#include "runtime/SessionHost.h"

#include <QJsonDocument>

namespace ketplus::daemon {
namespace {
QJsonObject error(const QString& code, const QString& message) {
    return {{"ok", false}, {"error_code", code}, {"error_message", message}};
}
} // namespace

QJsonObject SessionHost::listSessions(const QJsonObject& payload) const {
    const bool paged = payload.contains("deleted_sessions_page_version");
    if ((paged && payload.value("deleted_sessions_page_version").toInteger(-1) != 1) ||
        (payload.contains("deleted_sessions_only") &&
         !payload.value("deleted_sessions_only").isBool()) ||
        (payload.contains("deleted_sessions_after") &&
         (!paged || !payload.value("deleted_sessions_after").isObject() ||
          payload.value("deleted_sessions_after").toObject().isEmpty())))
        return error("invalid_tombstone_page", "Unsupported tombstone page or cursor.");
    const auto page =
        journal_->sessionTombstones(payload.value("deleted_sessions_after").toObject(), !paged);
    if (!page)
        return error("journal_error", journal_->errorString());
    if (!paged && !page->next.isEmpty())
        return error("session_catalog_upgrade_required",
                     "This deletion catalog requires a client with tombstone pagination.");
    QJsonObject catalog{{"ok", true}, {"deleted_sessions", page->tombstones}};
    if (paged) {
        catalog.insert("deleted_sessions_page_version", 1);
        catalog.insert("deleted_sessions_next", page->next);
        if (payload.contains("deleted_sessions_after") ||
            payload.value("deleted_sessions_only").toBool())
            return catalog; // A continuation never reloads the live session catalog.
    }

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
    // Leave room for the IPC response envelope; never knowingly send a frame
    // that the client must reject. Live-session pagination is a separate gate.
    if (QJsonDocument(catalog).toJson(QJsonDocument::Compact).size() >
        protocol::maximumFrameBytes - 16 * 1024)
        return error("session_catalog_too_large",
                     "The live session catalog exceeds its byte limit.");
    return catalog;
}

} // namespace ketplus::daemon
