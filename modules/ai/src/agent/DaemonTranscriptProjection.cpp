#include "agent/DaemonTranscriptProjection.h"
#include "agent/AgentMessageFormatter.h"
#include "agent/DaemonAgentNotes.h"
#include "agent/DaemonTranscriptNotices.h"
#include "agent/McpToolResultMedia.h"
#include "agent/TeamTranscriptBlocks.h"
#include "agent/TranscriptActivityGroups.h"
#include "agent/TranscriptNormalizer.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace ketplus::agent_message {

DaemonDisplayState daemonDisplayState(const QString& state) {
    if (state == "queued")
        return {QStringLiteral("Queued"), true};
    if (state == "running" || state == "starting")
        return {QStringLiteral("Working…"), true};
    // Older daemons left in-flight turns as "recovering" with no provider to resume them.
    if (state == "recovering")
        return {QStringLiteral("Stopped"), false};
    if (state == "stopping")
        return {QStringLiteral("Stopping…"), true};
    if (state == "waiting_for_input" || state == "waiting_for_approval")
        return {QStringLiteral("Waiting for your answer…"), true};
    if (state == "error" || state == "failed")
        return {QStringLiteral("Error"), false};
    if (state == "stopped" || state == "interrupted" || state == "orphaned")
        return {QStringLiteral("Stopped"), false};
    if (state == "ready" || state == "completed")
        return {QStringLiteral("Ready"), false};
    return {QStringLiteral("Session unavailable"), false};
}

QString daemonMessageKey(const qint64 turnSequence, const QString& itemId) {
    // An approval and a sent agent message's note follow their id across turns and pages.
    if (itemId.startsWith("approval:") || itemId.startsWith(agentNoteItemPrefix))
        return QStringLiteral("daemon:%1").arg(itemId);
    return QStringLiteral("daemon:%1:%2").arg(turnSequence).arg(itemId);
}

QString daemonTurnId(const qint64 turnSequence) {
    return QStringLiteral("daemon-turn:%1").arg(turnSequence);
}

QString daemonUserItemId(const qint64 cursor) { return QStringLiteral("user:%1").arg(cursor); }

QString daemonNoticeItemId(const qint64 cursor) { return QStringLiteral("notice:%1").arg(cursor); }

QString providerRequestDeclinedText(const QJsonObject& data) {
    const QString server = data.value(QStringLiteral("server_name")).toString();
    if (data.value(QStringLiteral("reason")).toString() ==
        QStringLiteral("unsupported_elicitation"))
        return server.isEmpty()
                   ? QStringLiteral("KetPlus declined an MCP request it cannot show yet.")
                   : QStringLiteral("KetPlus declined a request from the %1 MCP server that it "
                                    "cannot show yet.")
                         .arg(server);
    const QString method = data.value(QStringLiteral("method")).toString();
    return QStringLiteral("KetPlus declined a Codex request it does not support yet (%1).")
        .arg(method.isEmpty() ? QStringLiteral("unknown") : method);
}

QString providerProtocolErrorText(const QJsonObject& data) {
    const QString method = data.value(QStringLiteral("method")).toString();
    if (const qint64 bytes = data.value(QStringLiteral("oversized_bytes")).toInteger(); bytes > 0)
        return QStringLiteral("KetPlus skipped a %1 MiB Codex message (%2) that was too large to "
                              "read. Part of this turn may be missing.")
            .arg(bytes / (1024 * 1024))
            .arg(method.isEmpty() ? QStringLiteral("unknown") : method);
    QStringList missing;
    for (const auto& field : data.value(QStringLiteral("missing")).toArray())
        missing.append(field.toString());
    return QStringLiteral("KetPlus skipped a Codex message it could not read (%1 without %2). "
                          "Part of this turn may be missing.")
        .arg(method.isEmpty() ? QStringLiteral("unknown") : method,
             missing.isEmpty() ? QStringLiteral("a required field")
                               : missing.join(QStringLiteral(", ")));
}

QString providerProtocolErrorTitle() { return QStringLiteral("Protocol error"); }

QString providerRuntimeNoticeTitle() { return QStringLiteral("Provider version"); }

QString agentErrorTitle() { return QStringLiteral("The provider stopped with an error"); }

QString approvalToolLabel(const QJsonObject& details) {
    const QString type = details.value("request_type").toString();
    if (type == "command")
        return QStringLiteral("Command");
    if (type == "file_change")
        return QStringLiteral("File change");
    if (type == "mcp_tool_call") {
        const QString server = details.value("server_name").toString();
        const QString tool = details.value("tool_title").toString();
        if (!server.isEmpty() && !tool.isEmpty())
            return server + QLatin1Char('.') + tool;
        if (!server.isEmpty() || !tool.isEmpty())
            return (server.isEmpty() ? tool : server) + QStringLiteral(" tool");
    }
    if (type == "claude_tool")
        return QStringLiteral("Claude tool");
    if (type == "cursor_tool")
        return QStringLiteral("Cursor tool");
    return QStringLiteral("Tool");
}

QVariantMap joinedApprovalRows(const QVariantMap& older, const QVariantMap& newer) {
    const auto settled = [](const QVariantMap& row) {
        const auto status = row.value("status").toString();
        return status == "approved" || status == "denied" || status == "expired" ||
               status == "resolved";
    };
    // The request names the tool and command; a live card has its buttons.
    const auto detailed = [](const QVariantMap& row) {
        return !row.value("target").toString().isEmpty() ||
               !row.value("command").toString().isEmpty();
    };
    if (!settled(older) && !settled(newer)) {
        if (newer.value("running").toBool() || !older.value("running").toBool())
            return detailed(newer) || !detailed(older) ? newer : older;
        return older;
    }
    // The receipt is authoritative: a decided approval is never a pending card again.
    auto joined = detailed(newer) || !detailed(older) ? newer : older;
    joined.insert("status", settled(newer) ? newer.value("status") : older.value("status"));
    joined.insert("running", false);
    joined.insert("presentationHidden", !detailed(joined));
    return joined;
}

QString approvalOutcome(const QString& eventType, const QJsonObject& data) {
    if (eventType.endsWith(QStringLiteral(".expired")))
        return QStringLiteral("expired");
    const QString decision = data.value("answers").toObject().value("decision").toString();
    return decision == "accept"    ? QStringLiteral("approved")
           : decision == "decline" ? QStringLiteral("denied")
                                   : QStringLiteral("resolved");
}

void presentAgentError(QVariantMap& entry, const QString& message) {
    entry.insert(QStringLiteral("title"), agentErrorTitle());
    // The adapters' own words when a provider process ends under a turn.
    static const QList<QPair<QRegularExpression, QString>> exits{
        {QRegularExpression(QStringLiteral("^Codex app-server stopped \\(exit")),
         QStringLiteral("Codex CLI exited")},
        {QRegularExpression(QStringLiteral("^The Cursor ACP process exited unexpectedly")),
         QStringLiteral("Cursor CLI exited")},
        {QRegularExpression(QStringLiteral("^The Claude sidecar exited unexpectedly")),
         QStringLiteral("Claude Code exited")}};
    for (const auto& [pattern, title] : exits) {
        if (pattern.match(message).hasMatch()) {
            entry.insert(QStringLiteral("title"), title);
            return;
        }
    }
    // Only the providers' own overload errors: Codex's "Selected model is at capacity" and
    // Anthropic's 529 overloaded_error. A message is also the agent's last words when a turn
    // fails otherwise, so a word such as "overloaded" in it says nothing.
    static const QRegularExpression capacity(
        QStringLiteral("^Selected model is at capacity\\b|^API Error: "
                       "529\\b|\\boverloaded_error\\b|^Overloaded\\.?$"));
    if (capacity.match(message).hasMatch()) {
        entry.insert(QStringLiteral("title"), QStringLiteral("Selected model is at capacity"));
        entry.insert(QStringLiteral("subtype"), QStringLiteral("modelCapacity"));
        const QString text = QStringLiteral(
            "The model could not finish the turn. Try again, or pick another model for this "
            "conversation.");
        entry.insert(QStringLiteral("body"), text);
        entry.insert(QStringLiteral("summary"), text);
    }
}

QVariantMap providerNoticeMessage(const QString& text, const QString& itemId,
                                  const QString& title) {
    auto entry = message(QStringLiteral("tool"), text, QStringLiteral("Codex · ") + title.toLower(),
                         {}, itemId);
    entry.insert(QStringLiteral("category"), QStringLiteral("system"));
    entry.insert(QStringLiteral("subtype"), QStringLiteral("notice"));
    entry.insert(QStringLiteral("title"), title);
    entry.insert(QStringLiteral("summary"), text);
    entry.insert(QStringLiteral("running"), false);
    return entry;
}

void markAgentOrigin(QVariantMap& entry, const QJsonArray& agentMessages) {
    if (agentMessages.isEmpty())
        return;
    QStringList senders;
    bool reviews = true;
    for (const QJsonValue& value : agentMessages) {
        const QJsonObject message = value.toObject();
        const QString sender = QStringLiteral("@agent/") + message.value("from_number").toString();
        if (!senders.contains(sender))
            senders.append(sender);
        reviews = reviews && message.value("kind").toString() == QStringLiteral("review");
    }
    const QJsonObject first = agentMessages.first().toObject();
    // As the mock's "From @agent/8941 · DS Icon · Claude"; the sender's note says it replied.
    QString label = (reviews ? QStringLiteral("Review request from ") : QStringLiteral("From ")) +
                    senders.join(QStringLiteral(", "));
    if (senders.size() == 1) {
        if (const QString title = first.value("from_title").toString(); !title.isEmpty())
            label += QStringLiteral(" · ") + title;
        if (const QString provider = first.value("from_provider").toString(); !provider.isEmpty())
            label += QStringLiteral(" · ") + provider.left(1).toUpper() + provider.mid(1);
    }
    entry.insert(QStringLiteral("subtype"), QStringLiteral("agentMessage"));
    entry.insert(QStringLiteral("meta"), label);
    entry.insert(QStringLiteral("title"), label);
}

QString agentMessageNoticeTitle() { return QStringLiteral("Agent message"); }

QString agentMessageStateText(const QJsonObject& data) {
    const QString peer = QStringLiteral("@agent/") + data.value("peer_number").toString();
    const QString state = data.value("state").toString();
    const QString reason = data.value("reason").toString();
    const bool outgoing = data.value("direction").toString() == QStringLiteral("outgoing");
    const QString title = data.value("peer_title").toString();
    const QString named = title.isEmpty() ? peer : peer + QStringLiteral(" · ") + title;
    if (state == QStringLiteral("review_skipped")) {
        if (reason == QStringLiteral("no_changes"))
            return QStringLiteral("No review: this turn changed no files.");
        if (reason == QStringLiteral("round_limit"))
            return QStringLiteral("No review: this exchange used all its review rounds.");
        if (reason == QStringLiteral("reviewer_missing"))
            return QStringLiteral("No review: the reviewer conversation no longer exists.");
        if (reason == QStringLiteral("reviewer_busy"))
            return QStringLiteral("No review: the reviewer was busy with another turn.");
        if (reason == QStringLiteral("reviewer_unsupported"))
            return QStringLiteral("No review: only a Claude conversation can review turns.");
        if (reason == QStringLiteral("snapshot_failed"))
            return QStringLiteral("No review: KetPlus could not read this turn's changes. The "
                                  "next review includes them.");
        return QStringLiteral("The review request could not be sent.");
    }
    if (state == QStringLiteral("sent")) {
        if (data.value("kind").toString() == QStringLiteral("reply"))
            return QStringLiteral("Replied to %1.").arg(named);
        if (data.value("kind").toString() == QStringLiteral("review"))
            return QStringLiteral("Asked %1 to review this turn (round %2 of %3).")
                .arg(named)
                .arg(data.value("round").toInt())
                .arg(data.value("max_rounds").toInt());
        const QString delivery = data.value("delivery").toString();
        QString text = QStringLiteral("Sent to %1 · ").arg(named) +
                       (delivery == QStringLiteral("starting") ? QStringLiteral("started a turn")
                        : delivery == QStringLiteral("steered")
                            ? QStringLiteral("added to its running turn")
                            : QStringLiteral("delivered when its turn ends"));
        if (data.value("expect_reply").toBool())
            text += QStringLiteral(" · reply requested");
        return text + QLatin1Char('.');
    }
    if (state == QStringLiteral("replied"))
        return QStringLiteral("Sent as the reply to %1.").arg(named);
    if (state == "authorized")
        return outgoing ? QStringLiteral("%1 authorized receipt of your message.").arg(named)
                        : QStringLiteral("You authorized receipt of 1 message from %1.").arg(named);
    if (state == "releasing")
        return outgoing ? QStringLiteral("Delivering your message to %1.").arg(named)
                        : QStringLiteral("Receiving the authorized message from %1.").arg(named);
    if (state == "delivered")
        return outgoing ? QStringLiteral("Message delivered to %1.").arg(named)
                        : QStringLiteral("Received the authorized message from %1.").arg(named);
    if (state == QStringLiteral("held")) {
        const QString why = reason == QStringLiteral("hop_limit")
                                ? QStringLiteral("the agents exchanged too many messages")
                            : reason == QStringLiteral("chain_limit")
                                ? QStringLiteral("this exchange started too many turns")
                                : QStringLiteral("too many automatic turns ran this hour");
        return outgoing
                   ? QStringLiteral("Message to %1 is held because %2. It is delivered with "
                                    "that conversation's next message from you.")
                         .arg(peer, why)
                   : QStringLiteral("A message from %1 is held because %2. Send a message here "
                                    "to read it.")
                         .arg(peer, why);
    }
    if (state == QStringLiteral("expired"))
        return outgoing ? QStringLiteral("%1 did not reply within 24 hours.").arg(peer)
                        : QStringLiteral("The message from %1 has expired.").arg(named);
    if (reason == QStringLiteral("no_reply"))
        return QStringLiteral("%1 finished without a reply.").arg(peer);
    if (reason == QStringLiteral("reviewer_unsupported"))
        return QStringLiteral("%1 no longer runs on Claude, so it did not review this turn.")
            .arg(named);
    if (reason.startsWith(QStringLiteral("turn_")))
        return QStringLiteral("%1 stopped before replying (%2).").arg(peer, reason.mid(5));
    return (outgoing ? QStringLiteral("Message to %1 failed: %2")
                     : QStringLiteral("Receiving a message from %1 failed: %2"))
        .arg(peer, reason);
}

QVariantMap agentMessageNotice(const QJsonObject& data, const QString& itemId) {
    // ADR 0067: a review that starts no turn reads like the reviewer's message.
    if (data.value("state").toString() == QStringLiteral("review_recorded")) {
        QString label =
            QStringLiteral("Review from @agent/") + data.value("peer_number").toString();
        if (const QString title = data.value("peer_title").toString(); !title.isEmpty())
            label += QStringLiteral(" · ") + title;
        label += data.value("verdict").toString() == QStringLiteral("no_findings")
                     ? QStringLiteral(" · No findings")
                     : QStringLiteral(" · Findings");
        auto entry =
            message(QStringLiteral("user"), data.value("body").toString(), label, {}, itemId);
        entry.insert(QStringLiteral("subtype"), QStringLiteral("agentMessage"));
        entry.insert(QStringLiteral("title"), label);
        return entry;
    }
    const bool review = data.value("kind").toString() == QStringLiteral("review");
    auto entry =
        providerNoticeMessage(agentMessageStateText(data), itemId,
                              review ? QStringLiteral("Review") : agentMessageNoticeTitle());
    if (data.value("direction").toString() == "incoming" &&
        !data.value("message_id").toString().isEmpty()) {
        entry.insert("subtype", "heldAgentMessage");
        entry.insert("sanitizedPayload",
                     QVariantMap{{"messageId", data.value("message_id").toString()}});
    }
    return entry;
}

void labelProvider(QVariantMap& entry, const QString& provider) {
    if (provider.isEmpty() || provider == QStringLiteral("codex"))
        return;
    const QString meta = entry.value(QStringLiteral("meta")).toString();
    if (!meta.startsWith(QStringLiteral("Codex")))
        return;
    const QString name = provider == QStringLiteral("claude")
                             ? QStringLiteral("Claude")
                             : provider.left(1).toUpper() + provider.mid(1);
    entry.insert(QStringLiteral("meta"), name + meta.mid(5));
}

namespace {

// Claude sidecars before the snapshot fix (#105) journaled an answer that followed thinking
// twice: once under its stream index ("msg_X-1") and once under its index in
// Claude Code's per-block snapshot ("msg_X-0"). Returns the streamed row a
// completed snapshot of the same message and text belongs to, if any.
QString claudeSnapshotTwin(const QHash<QString, QStringList>& claudeTextRows,
                           const QHash<QString, int>& indexes, const QVariantList& rows,
                           const qint64 turnSequence, const QString& itemId, const QString& text) {
    const auto separator = itemId.lastIndexOf(QLatin1Char('-'));
    if (!itemId.startsWith(QStringLiteral("msg_")) || separator < 0 || text.isEmpty())
        return {};
    const auto candidates =
        claudeTextRows.value(daemonMessageKey(turnSequence, itemId.left(separator)));
    for (const QString& candidate : candidates) {
        const auto row = rows.at(indexes.value(candidate)).toMap();
        if (row.value("kind") == "assistant" && row.value("body").toString() == text)
            return candidate;
    }
    return {};
}

} // namespace

QVariantMap daemonActivityGroupMessage(const QJsonObject& data, qint64 turn) {
    const auto id = data.value("item_id").toString();
    auto row = message("tool", {}, "Agent", {}, id);
    // Worded as the local groups (TranscriptActivityGroups), from the daemon's family counts.
    const auto files = data.value("files").toArray();
    ActivityTally tally;
    for (const auto& value : data.value("families").toArray()) {
        const auto part = value.toObject();
        const auto family = part.value("family").toString();
        const auto count = part.value("count").toInt();
        if (family == "edit") {
            // A group's edits count the files they changed, as a local group does.
            tally.add(family, files.isEmpty() ? count : data.value("files_total").toInt(count));
        } else if (family == "list" || family.startsWith("list:")) {
            // "list:2" is a command that listed two folders.
            const int folders = family == "list" ? 1 : std::max(1, family.mid(5).toInt());
            tally.add(QStringLiteral("list"), count * folders);
        } else if (family == "command" || family == "read" || family == "codeSearch" ||
                   family == "fileSearch" || family == "wait" || family == "thought" ||
                   family == "webFetch" || family == "toolsLoaded" || family == "skill" ||
                   family == "taskStopped") {
            tally.add(family, count);
        } else if (family == "webSearch") {
            tally.add(QStringLiteral("search"), count);
        } else if (family == "search") {
            // Indexed before 2026-11-09: web, code and file searches alike.
            tally.add(QStringLiteral("searched"), count);
        } else if (family == "browser") {
            tally.addServer(QStringLiteral("ketplus_browser"), count);
        } else if (family.startsWith("tool:")) {
            tally.addServer(family.mid(5), count);
        } else if (family == "subagent") {
            tally.add(family, count);
            tally.subagents += count;
        } else if (family == "subagent.step") {
            tally.add(QStringLiteral("subagent"), count);
        } else {
            tally.add(QStringLiteral("tool"), count);
        }
    }
    // Totals only when every file is listed and counted, as the changed-files card shows them.
    tally.statsKnown =
        !files.isEmpty() && data.value("files_total").toInt(int(files.size())) == files.size();
    for (const auto& value : files) {
        const auto file = value.toObject();
        if (!file.value("known").toBool(true))
            tally.statsKnown = false;
        tally.added += file.value("added").toInt();
        tally.removed += file.value("removed").toInt();
    }
    tally.failed = data.value("failed_count").toInt();
    row.insert("category", "activity");
    row.insert("subtype", "daemonGroup");
    row.insert("turnId", daemonTurnId(turn));
    row.insert("messageKey", daemonMessageKey(turn, id));
    row.insert("daemonGroupId", data.value("group_id").toString());
    row.insert("groupCursor", data.value("last_change_cursor").toInteger());
    row.insert("groupMembership", data.value("membership_cursor").toInteger());
    row.insert("groupKey", row.value("messageKey"));
    row.insert("groupLeaderKey", row.value("messageKey"));
    row.insert("groupRole", "leader");
    row.insert("groupSummary", activityTallySummary(tally));
    row.insert("groupSize", data.value("count").toInt());
    row.insert("groupRunning", data.value("running_count").toInt() > 0);
    row.insert("groupFailed", data.value("failed_count").toInt());
    // ADR 0022: the files the group's edits changed, for the turn's changed-files card.
    row.insert("groupFiles", files.toVariantList());
    row.insert("groupFilesTotal", data.value("files_total").toInt(int(files.size())));
    row.insert("running", false);
    return row;
}

QVariantList daemonWindowMessages(const QJsonObject& page) {
    QVariantList result;
    QHash<QString, int> indexes;
    QHash<QString, QString> settledQuestions;
    QHash<QString, QString> settledApprovals;
    QSet<QString> requestedApprovals;
    QHash<QString, QVariantMap> questionAnswers;
    QSet<QString> partial;
    const auto identity = daemonMessageKey;
    QSet<QString> hiddenItems;
    // Claude text rows by turn and message id, for claudeSnapshotTwin.
    QHash<QString, QStringList> claudeTextRows;
    for (const auto& value : page.value("events").toArray()) {
        const auto event = value.toObject();
        const auto data = event.value("payload").toObject();
        const auto item = data.value("item").toObject();
    }
    for (const auto& value : page.value("partial_items").toArray()) {
        const auto item = value.toObject();
        partial.insert(
            identity(item.value("turn_sequence").toInteger(), item.value("item_id").toString()));
    }
    // Settled input requests are safe transcript history. Pending approval
    // ownership is synchronized separately and must never be reconstructed
    // from journal history.
    for (const auto& value : page.value("events").toArray()) {
        const auto event = value.toObject();
        const auto type = event.value("event_type").toString();
        if (type == "approval.resolved" || type == "approval.expired") {
            const auto data = event.value("payload").toObject();
            settledApprovals.insert(data.value("approval_id").toString(),
                                    approvalOutcome(type, data));
            continue;
        }
        if (type == "approval.requested" &&
            event.value("payload").toObject().value("kind") != "input")
            requestedApprovals.insert(
                event.value("payload").toObject().value("approval_id").toString());
        if (type != "question.answered" && type != "question.expired")
            continue;
        const auto data = event.value("payload").toObject();
        QString id = data.value("approval_id").toString();
        if (id.isEmpty())
            id = data.value("request_id").toString();
        if (!id.isEmpty())
            questionAnswers.insert(id, data.value("answers").toObject().toVariantMap());
        if (!id.isEmpty())
            settledQuestions.insert(id, type == "question.expired" ? QStringLiteral("expired")
                                                                   : QStringLiteral("resolved"));
    }
    QJsonObject sessionUsage;
    // A turn's start and the context its own usage reported, for its end line. A page that
    // begins mid-turn has no start, so that turn's line omits the duration.
    QHash<qint64, QDateTime> turnStarts;
    QHash<qint64, qint64> turnContext;
    // ADR 0106: a turn's last work report, shown with its end row when turn.completed does not
    // carry it.
    QHash<qint64, QJsonObject> turnReports;
    for (const auto& value : page.value("events").toArray()) {
        const auto event = value.toObject();
        const auto type = event.value("event_type").toString();
        const auto data = event.value("payload").toObject();
        const auto sequence = event.value("turn_sequence").toInteger();
        QString itemId = data.value("item_id").toString();
        if (type == "usage.session")
            sessionUsage = data;
        if (type == "usage.context")
            turnContext.insert(sequence, daemonContextTokens(data));
        const QDateTime sentAt =
            QDateTime::fromString(event.value("sent_at").toString(), Qt::ISODateWithMs);
        if (daemonTurnStarts(type) && sentAt.isValid() && !turnStarts.contains(sequence))
            turnStarts.insert(sequence, sentAt);
        TurnFacts turn;
        turn.contextTokens = turnContext.value(sequence, -1);
        if (type == "turn.completed" && sentAt.isValid() && turnStarts.contains(sequence))
            turn.durationMs = turnStarts.value(sequence).msecsTo(sentAt);
        if (type == "work_report.updated") {
            turnReports.insert(sequence, data);
            continue;
        }
        auto notice = daemonEventNotice(
            type, data, daemonNoticeItemId(event.value("cursor").toInteger()), sessionUsage, turn);
        // The daemon repeats the report on turn.completed, so a page that starts after the
        // turn's report events still shows it.
        if (type == "turn.completed" && !notice.isEmpty()) {
            const QJsonObject report = data.contains("work_report")
                                           ? data.value("work_report").toObject()
                                           : turnReports.value(sequence);
            if (!report.isEmpty())
                notice.insert("workReport", report.toVariantMap());
        }
        QJsonObject item = data.value("item").toObject();
        if (item.isEmpty()) {
            item = data;
            item.insert("type", data.value("item_type"));
            item.insert("id", itemId);
        } else
            itemId = item.value("id").toString();
        if (!notice.isEmpty())
            itemId = notice.value("itemId").toString();
        if (type == "user.message")
            itemId = daemonUserItemId(event.value("cursor").toInteger());
        if (type == "runtime.error" && notice.isEmpty())
            itemId = QStringLiteral("error:%1").arg(event.value("cursor").toInteger());
        if (type == "provider.request_declined" || type == "provider.runtime" ||
            type == "provider.protocol_error" || type == "agent_message.state")
            itemId = daemonNoticeItemId(event.value("cursor").toInteger());
        if (type == "agent_message.state" && !agentNoteItemId(data).isEmpty())
            itemId = agentNoteItemId(data);
        // Steer text is journaled by current daemons; older empty steers stay metadata.
        if (type == "turn.steered")
            itemId = data.value("text").toString().isEmpty()
                         ? QString()
                         : QStringLiteral("steer:%1").arg(event.value("cursor").toInteger());
        const bool settledQuestion =
            type == "approval.requested" && data.value("kind") == "input" &&
            settledQuestions.contains(data.value("approval_id").toString()) &&
            data.value("questions").isArray();
        // A settled approval stays in the history, as the mock's Approved line. A pending one
        // arrives live, with its buttons. A page holding only the request or only its receipt
        // keeps a hidden half that mergeDaemonHistory joins with the other page's.
        const QString approvalId = data.value("approval_id").toString();
        // An approval without an id cannot be joined or answered, so it stays out.
        const bool approvalRequest =
            type == "approval.requested" && data.value("kind") != "input" && !approvalId.isEmpty();
        const bool approvalReceipt = (type == "approval.resolved" || type == "approval.expired") &&
                                     !approvalId.isEmpty() &&
                                     !requestedApprovals.contains(approvalId);
        if (approvalRequest || approvalReceipt)
            itemId = QStringLiteral("approval:%1").arg(approvalId);
        if (type == "approval.requested" && !settledQuestion && !approvalRequest)
            continue;
        const bool answerReceipt = type == "question.answered" && data.value("questions").isArray();
        if (settledQuestion || answerReceipt) {
            const QString approvalId = data.value("approval_id").toString();
            itemId = QStringLiteral("approval:%1").arg(approvalId);
        }
        if (itemId.isEmpty())
            continue;
        auto key = identity(sequence, itemId);
        if (hiddenItems.contains(key))
            continue;
        int existing = indexes.value(key, -1);
        if (existing < 0 && type == "item.completed" && item.value("type") == "agentMessage") {
            const auto twin = claudeSnapshotTwin(claudeTextRows, indexes, result, sequence, itemId,
                                                 item.value("text").toString());
            if (!twin.isEmpty()) {
                key = twin;
                existing = indexes.value(twin);
                itemId = result.at(existing).toMap().value("itemId").toString();
                item.insert("id", itemId);
            }
        }
        QVariantMap entry;
        if (!notice.isEmpty()) {
            entry = notice;
        } else if (type == "activity.group.summary") {
            entry = daemonActivityGroupMessage(data, sequence);
        } else if (approvalRequest || approvalReceipt) {
            entry = message("tool", approvalRequest ? data.value("reason").toString() : QString{},
                            "Agent · approval", {}, itemId);
            entry.insert("subtype", "approval");
            if (approvalRequest) {
                entry.insert("command", data.value("command").toString());
                entry.insert("target", approvalToolLabel(data));
            }
            entry.insert("running", false);
            entry.insert("status", settledApprovals.value(approvalId, QStringLiteral("requested")));
            entry.insert("presentationHidden",
                         !approvalRequest || !settledApprovals.contains(approvalId));
        } else if (type == "approval.requested" || answerReceipt) {
            entry = message("tool", {}, "Codex · question ended", {}, itemId);
            entry.insert("subtype", "question");
            auto questions = data.value("questions").toArray();
            for (qsizetype i = 0; i < questions.size(); ++i) {
                auto question = questions.at(i).toObject();
                if (!question.contains("question"))
                    question.insert("question", question.value("prompt"));
                questions[i] = question;
            }
            entry.insert("questions", questions.toVariantList());
            entry.insert("questionAnswers",
                         questionAnswers.value(data.value("approval_id").toString()));
            entry.insert("running", false);
            entry.insert("status", settledQuestions.value(data.value("approval_id").toString()));
        } else if (type == "runtime.error") {
            entry = message("error", data.value("message").toString(), "Agent · error", {}, itemId);
            // Worded as a live failure's row, as the mock's danger banner.
            presentAgentError(entry, data.value("message").toString());
            if (const QString code = data.value("code").toString(); !code.isEmpty()) {
                // The provider's setup guide (ADR 0110), which quotes the provider's own words.
                entry.insert("errorCode", code);
                entry.insert("errorDetail", data.value("message").toString());
                entry.insert("errorModel", data.value("model").toString());
                entry.insert("provider", data.value("provider").toString());
            }
            entry.insert("category", "system");
            entry.insert("status", "failed");
        } else if (type == "command.output_delta") {
            entry = existing < 0 ? commandMessage(QJsonObject{{"id", itemId},
                                                              {"type", "commandExecution"},
                                                              {"status", "inProgress"}},
                                                  false)
                                 : result.at(existing).toMap();
            if (existing < 0)
                entry.insert("partialOutput", true);
            auto details = entry.value("details").toMap();
            details.insert("output", TranscriptSanitizer::text((details.value("output").toString() +
                                                                data.value("delta").toString())
                                                                   .right(128 * 1024)));
            entry.insert("details", details);
        } else if (type == "provider.request_declined") {
            entry = providerNoticeMessage(providerRequestDeclinedText(data), itemId);
        } else if (type == "provider.protocol_error") {
            entry = providerNoticeMessage(providerProtocolErrorText(data), itemId,
                                          providerProtocolErrorTitle());
        } else if (type == "provider.runtime") {
            entry = providerNoticeMessage(data.value("message").toString(), itemId,
                                          providerRuntimeNoticeTitle());
        } else if (type == "agent_message.state") {
            entry = agentNote(data, itemId);
            // A later state of the message updates its note where it first appeared.
            if (!entry.isEmpty() && existing >= 0) {
                result[existing] = mergedAgentNote(result.at(existing).toMap(), entry);
                continue;
            }
            if (entry.isEmpty())
                entry = agentMessageNotice(data, itemId);
        } else if (type == "user.message" || type == "turn.steered") {
            // A steer carries its images the way the turn's first message does.
            entry = message("user", data.value("text").toString(), "You", {}, itemId);
            entry.insert("images", mcp_media::messageImages(data.value("images").toArray()));
            if (type == "turn.steered") {
                entry.insert("eventType", "turn.steered");
                entry.insert("provider", data.value("provider").toString());
            }
            if (data.value("origin").toString() == QStringLiteral("agent")) {
                markAgentOrigin(entry, data.value("agent_messages").toArray());
                // #468: a team's own message is drawn as its block; the text stays the body.
                if (const QVariantMap block =
                        team_blocks::fromAgentMessages(data.value("agent_messages").toArray());
                    !block.isEmpty()) {
                    entry.insert(QStringLiteral("subtype"), QStringLiteral("teamMessage"));
                    entry.insert(QStringLiteral("teamBlock"), block);
                }
                // The reply answers a note above it, which now reads Replied; a note in
                // another page is marked when the pages merge.
                const auto answered = agentRepliesTo(data.value("agent_messages").toArray());
                entry.insert("agentRepliesTo", answered);
                for (const auto& id : answered)
                    if (const int note = indexes.value(agentNoteKey(id), -1); note >= 0)
                        result[note] = repliedAgentNote(result.at(note).toMap());
            }
        } else if (type == "assistant.delta") {
            entry = existing < 0 ? message("assistant", {}, "Codex · streaming", {}, itemId)
                                 : result.at(existing).toMap();
            entry.insert("body", entry.value("body").toString() + data.value("delta").toString());
            if (existing < 0)
                entry.insert("partialBody", partial.contains(key));
        } else if (type == "item.started" || type == "item.completed") {
            // The daemon's own user.message row is authoritative; Codex echoes the prompt as a
            // userMessage item, which would otherwise render the prompt twice.
            if (item.value("type") == "userMessage")
                continue;
            if (type == "item.completed" && item.value("type") == "agentMessage" &&
                !item.value("text").isString()) {
                entry = existing < 0 ? message("assistant", {}, "Codex", {}, itemId)
                                     : result.at(existing).toMap();
                if (existing < 0)
                    entry.insert("partialBody", partial.contains(key));
            } else if (item.value("type") != "agentMessage") {
                // Normalizing sanitizes the whole item, its output included, so
                // it runs once: the lifecycle phase decides the row, and the
                // completed projection has the same visibility.
                const auto activity = ActivityNormalizer::normalize(
                    item,
                    type == "item.started" ? TranscriptPhase::Started : TranscriptPhase::Completed);
                if (!activity.visible)
                    continue;
                entry = activity.toVariantMap();
                // A thought's duration runs from its start to its completion event.
                if (item.value("type") == "reasoning" && sentAt.isValid()) {
                    const qint64 startedAt =
                        existing < 0
                            ? -1
                            : result.at(existing).toMap().value("startedAt", -1).toLongLong();
                    // Both ends are kept: a later page or live event may bring the other one,
                    // and mergeDaemonHistory() times the thought then.
                    if (type == "item.started") {
                        entry.insert("startedAt", sentAt.toMSecsSinceEpoch());
                    } else {
                        entry.insert("completedAt", sentAt.toMSecsSinceEpoch());
                        if (startedAt >= 0) {
                            entry.insert("startedAt", startedAt);
                            entry.insert("durationMs", sentAt.toMSecsSinceEpoch() - startedAt);
                        }
                    }
                }
            } else {
                QSet<QString> seen;
                const auto projected =
                    historyMessages(QJsonArray{QJsonObject{{"id", QString::number(sequence)},
                                                           {"items", QJsonArray{item}}}},
                                    seen);
                if (projected.isEmpty())
                    continue;
                entry = projected.first().toMap();
            }
            if (type == "item.started") {
                if (entry.value("kind") == "assistant") {
                    entry.insert("meta", "Codex · streaming");
                    if (existing >= 0 && entry.value("body").toString().isEmpty())
                        entry.insert("body", result.at(existing).toMap().value("body"));
                    entry.insert("partialBody", partial.contains(key));
                } else if (entry.value("kind") == "tool") {
                    entry.insert("running", true);
                    if (entry.value("subtype") == "command")
                        entry.insert("meta", "Command · running");
                }
            }
        } else
            continue; // Includes command output and non-transcript lifecycle events.
        if (type == "user.message")
            entry.insert("timestamp", event.value("sent_at").toString());
        entry.insert("itemId", itemId);
        entry.insert("turnId", daemonTurnId(sequence));
        entry.insert("messageKey", key);
        if (entry.value("partialBody").toBool())
            entry.insert("meta", "Codex · Earlier text not loaded");
        labelProvider(entry, data.value("provider").toString());
        if (existing < 0) {
            indexes.insert(key, result.size());
            result.append(entry);
            const auto separator = itemId.lastIndexOf(QLatin1Char('-'));
            if (entry.value("kind") == "assistant" && itemId.startsWith(QStringLiteral("msg_")) &&
                separator > 0)
                claudeTextRows[identity(sequence, itemId.left(separator))].append(key);
        } else
            result[existing] = entry;
    }
    return result;
}

QVariantList mergeDaemonHistory(const QVariantList& older, const QVariantList& newer) {
    QVariantList result = older;
    QHash<QString, int> indexes;
    for (int index = 0; index < result.size(); ++index)
        indexes.insert(result.at(index).toMap().value("messageKey").toString(), index);
    for (const auto& value : newer) {
        auto entry = value.toMap();
        const auto key = entry.value("messageKey").toString();
        const int index = indexes.value(key, -1);
        // A reply in this page answers a sent message's note in the other.
        for (const auto& id : entry.value("agentRepliesTo").toStringList())
            if (const int note = indexes.value(agentNoteKey(id), -1); note >= 0)
                result[note] = repliedAgentNote(result.at(note).toMap());
        if (index < 0) {
            indexes.insert(key, result.size());
            result.append(entry);
            continue;
        }
        const auto prefix = result.at(index).toMap();
        if (prefix.value("subtype") == "approval" && entry.value("subtype") == "approval") {
            result[index] = joinedApprovalRows(prefix, entry);
            continue;
        }
        // Two pages' states of one sent message: the later one, with what only the first knew.
        if (prefix.value("subtype") == "agentNote" && entry.value("subtype") == "agentNote") {
            result[index] = mergedAgentNote(prefix, entry);
            continue;
        }
        if (entry.value("partialOutput").toBool()) {
            auto details = entry.value("details").toMap();
            details.insert("output", (prefix.value("details").toMap().value("output").toString() +
                                      details.value("output").toString())
                                         .right(128 * 1024));
            // A suffix-only output row inherits the command metadata from the older page.
            auto combined = prefix;
            combined.insert("details", details);
            combined.insert("partialOutput", prefix.value("partialOutput"));
            entry = combined;
        }
        if (entry.value("partialBody").toBool()) {
            auto renderedPrefix = prefix.value("body").toString();
            renderedPrefix.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            entry.insert("bodyPrefixCharacters",
                         entry.value("bodyPrefixCharacters").toLongLong() + renderedPrefix.size());
            entry.insert("body", prefix.value("body").toString() + entry.value("body").toString());
            entry.insert("partialBody", prefix.value("partialBody", false));
            if (!entry.value("partialBody").toBool())
                entry.insert("meta", prefix.value("meta"));
        }
        // A thought's start and completion can arrive in different pages or live events.
        if (!entry.contains("startedAt") && prefix.contains("startedAt")) {
            entry.insert("startedAt", prefix.value("startedAt"));
            const auto duration = entry.value("durationMs");
            if (entry.contains("completedAt") && (!duration.isValid() || duration.isNull()))
                entry.insert("durationMs", entry.value("completedAt").toLongLong() -
                                               prefix.value("startedAt").toLongLong());
        }
        // A newer completion is authoritative, not another text fragment.
        result[index] = entry;
    }
    return result;
}

} // namespace ketplus::agent_message
