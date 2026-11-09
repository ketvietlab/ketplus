#include "agent/DaemonTranscriptNotices.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/TranscriptSanitizer.h"
#include <cmath>

namespace ketplus::agent_message {
namespace {
// A provider's name as the mock writes it: Claude, Codex, Cursor.
QString providerName(const QString& provider) {
    if (provider.isEmpty())
        return QStringLiteral("another provider");
    return provider.left(1).toUpper() + provider.mid(1);
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
    return parts.join(QStringLiteral(" · "));
}
QVariantMap daemonEventNotice(const QString& type, const QJsonObject& data, const QString& itemId,
                              const QJsonObject& usage, const TurnFacts& turn) {
    QString title, detail, subtype = "event";
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
        // The journal intentionally carries revisions, not a machine path.
        title = "Moved to another worktree";
    } else if (type == "session.forked") {
        title = "Forked from another conversation";
        detail = "Its history comes along";
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
        detail = turnEndText(turn, usage);
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
    return entry;
}
} // namespace ketplus::agent_message
