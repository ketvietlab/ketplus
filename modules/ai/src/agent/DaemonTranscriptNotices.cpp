#include "agent/DaemonTranscriptNotices.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/ProviderDisplay.h"
#include "agent/TranscriptSanitizer.h"
#include <cmath>

namespace ketplus::agent_message {
namespace {
// A provider's name as the mock writes it: Claude, Codex, Cursor.
QString providerName(const QString& provider) {
    if (provider.isEmpty())
        return QStringLiteral("another provider");
    if (provider == QStringLiteral("copilot"))
        return QStringLiteral("GitHub Copilot");
    return provider_display::name(provider);
}
// What retention reduced in a turn, as the mock's note: "This turn is older than 30 days:
// 38 commands and 12 tool results were reduced to their summaries (1.2 MB)."
QString trimmedDetailText(const QJsonObject& trimmed) {
    QStringList kinds;
    qint64 total = 0;
    const auto count = [&](const char* key, const char* one, const char* many) {
        const auto value = trimmed.value(QLatin1String(key)).toInteger();
        if (value <= 0)
            return;
        total += value;
        kinds.append(value == 1 ? QStringLiteral("1 ") + QLatin1String(one)
                                : QStringLiteral("%1 ").arg(value) + QLatin1String(many));
    };
    count("commands", "command", "commands");
    count("diffs", "file diff", "file diffs");
    count("tool_results", "tool result", "tool results");
    count("tool_inputs", "tool input", "tool inputs");
    count("thoughts", "thought", "thoughts");
    if (kinds.isEmpty())
        return {};
    QString list = kinds.takeLast();
    if (!kinds.isEmpty())
        list = kinds.join(QStringLiteral(", ")) + QStringLiteral(" and ") + list;
    const auto days = trimmed.value("older_than_days").toInteger();
    QString text = days > 0 ? QStringLiteral("This turn is older than %1 %2: ")
                                  .arg(days)
                                  .arg(days == 1 ? QStringLiteral("day") : QStringLiteral("days"))
                            : QStringLiteral("To save space, ");
    text += list + (total == 1 ? QStringLiteral(" was reduced to its summary")
                               : QStringLiteral(" were reduced to their summaries"));
    const auto bytes = trimmed.value("bytes").toInteger();
    if (bytes >= 1024 * 1024)
        text += QStringLiteral(" (%1 MB)").arg(double(bytes) / (1024 * 1024), 0, 'f', 1);
    else if (bytes >= 1024)
        text += QStringLiteral(" (%1 KB)").arg((bytes + 512) / 1024);
    return text + QLatin1Char('.');
}
} // namespace

QString sessionEstimateText(const QJsonObject& usage) {
    const auto cost = usage.value("estimatedCostUsd");
    if (!cost.isDouble() || !std::isfinite(cost.toDouble()) || cost.toDouble() < 0)
        return {};
    // The SDK reports a cumulative session estimate, never a per-turn charge.
    return QStringLiteral("Session estimate: $%1 USD").arg(cost.toDouble(), 0, 'f', 4);
}
bool daemonTurnStarts(const QString& type) {
    return type == "user.message" || type == "turn.started";
}

qint64 daemonContextTokens(const QJsonObject& data) {
    return data.value("tokenUsage")
        .toObject()
        .value("last")
        .toObject()
        .value("totalTokens")
        .toInteger(-1);
}

QString turnEndText(const TurnFacts& turn, const QJsonObject& usage) {
    QStringList parts;
    if (turn.durationMs >= 0) {
        const qint64 seconds = (turn.durationMs + 500) / 1000;
        parts.append(seconds < 60     ? QStringLiteral("%1s").arg(seconds)
                     : seconds < 3600 ? QStringLiteral("%1m %2s")
                                            .arg(seconds / 60)
                                            .arg(seconds % 60, 2, 10, QLatin1Char('0'))
                                      : QStringLiteral("%1h %2m")
                                            .arg(seconds / 3600)
                                            .arg(seconds % 3600 / 60, 2, 10, QLatin1Char('0')));
    }
    if (turn.contextTokens > 0)
        parts.append(turn.contextTokens < 1000
                         ? QStringLiteral("%1 context").arg(turn.contextTokens)
                         : QStringLiteral("%1k context").arg((turn.contextTokens + 500) / 1000));
    // The SDK reports a cumulative session estimate, never a per-turn charge, so it says so.
    const auto cost = usage.value("estimatedCostUsd");
    if (cost.isDouble() && std::isfinite(cost.toDouble()) && cost.toDouble() >= 0)
        parts.append(QStringLiteral("$%1 session").arg(cost.toDouble(), 0, 'f', 2));
    if (turn.stoppedByUser)
        parts.append(QStringLiteral("stopped by you"));
    return parts.join(QStringLiteral(" · "));
}
QVariantMap daemonEventNotice(const QString& type, const QJsonObject& data, const QString& itemId,
                              const QJsonObject& usage, const TurnFacts& turn) {
    QString title, detail, trimmed, subtype = "event";
    TurnFacts facts = turn;
    QString status = "completed";
    // As the mock words its events (TranscriptFixtures.qml).
    if (type == "session.provider_changed") {
        title = QStringLiteral("Switched from %1 to %2")
                    .arg(providerName(data.value("from").toString()),
                         providerName(data.value("to").toString()));
        detail = "The conversation history comes along";
    } else if (type == "session.configuration_changed") {
        const auto effective = data.value("effective").toObject();
        QString model = effective.value("model").toString();
        const auto effort = effective.value("effort").toString();
        if (!effort.isEmpty())
            model += QStringLiteral(" · ") + effort;
        // The event also follows permission, mode and context window changes; journals from
        // before `changed` existed cannot tell them apart, so they stay neutral.
        const auto changed = data.value("changed").toArray();
        const auto only = [&changed](const char* setting) {
            return changed.size() == 1 && changed.first().toString() == QLatin1String(setting);
        };
        if (changed.contains(QStringLiteral("model")) || changed.contains(QStringLiteral("effort")))
            title = model.isEmpty() ? QStringLiteral("Model changed")
                                    : QStringLiteral("Model changed to ") + model;
        else if (only("execution_policy"))
            title = "Permissions changed";
        else if (only("mode"))
            title = "Mode changed";
        else if (only("compact_window"))
            title = "Context window changed";
        else
            title = "Settings changed";
        if (!data.contains("changed"))
            detail = model;
    } else if (type == "conversation.context_changed") {
        // The journal carries the branch and the folder's name, never the machine path.
        // Journals from before they were recorded only know that the worktree changed.
        const auto branch = data.value("branch").toString();
        const auto folder = data.value("worktree_name").toString();
        title = branch.isEmpty() ? QStringLiteral("Moved to another worktree")
                                 : QStringLiteral("Moved to worktree ") + branch;
        if (!folder.isEmpty())
            detail = QStringLiteral("The agent now works in ") + folder;
    } else if (type == "session.forked") {
        const auto number = data.value("from_conversation_number").toString();
        const auto turn = data.value("through_turn").toInteger();
        title = number.isEmpty() ? QStringLiteral("Forked from another conversation")
                                 : QStringLiteral("Forked from @agent/") + number;
        if (!number.isEmpty() && turn > 0)
            title += QStringLiteral(" at turn %1").arg(turn);
        // The fork's own provider and model, as the mock's "Cursor · Composer 2".
        const auto provider = data.value("provider").toString();
        const auto model = data.value("model").toString();
        detail = provider.isEmpty()
                     ? QStringLiteral("Its history comes along")
                     : providerName(provider) + (model.isEmpty() ? QString{} : " · " + model);
    } else if (type == "session.renamed") {
        title = "Conversation renamed";
        detail = data.value("title").toString();
    } else if (type == "turn.completed") {
        subtype = "turnEnd";
        status = data.value("status").toString();
        if (status == "failed")
            title = "Turn failed";
        else if (status == "interrupted" || status == "stopped")
            title = "Turn stopped";
        else if (status == "completed")
            title = "Turn completed";
        else
            return {}; // Do not invent success for an unknown provider state.
        facts.stoppedByUser =
            title == "Turn stopped" && data.value("reason").toString() == QLatin1String("user");
        detail = turnEndText(facts, usage);
        trimmed = trimmedDetailText(data.value("detail_trimmed").toObject());
    } else if (type == "runtime.error" &&
               (data.value("will_retry").toBool() || data.value("willRetry").toBool())) {
        title = "Reconnecting";
        detail = data.value("message").toString();
        subtype = "reconnecting";
        status = "running";
    } else
        return {};
    auto entry = providerNoticeMessage(TranscriptSanitizer::text(detail), itemId, title);
    entry.insert("subtype", subtype);
    entry.insert("eventType", type);
    entry.insert("status", status);
    // A live end is worded again once the session's cost is known; it keeps the Stop.
    if (facts.stoppedByUser)
        entry.insert("stoppedByUser", true);
    if (!trimmed.isEmpty())
        entry.insert("trimmedDetail", trimmed);
    return entry;
}
} // namespace ketplus::agent_message
