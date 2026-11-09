#include "agent/DaemonTranscriptNotices.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/TranscriptSanitizer.h"
#include <cmath>

namespace ketplus::agent_message {
QString sessionEstimateText(const QJsonObject& usage) {
    const auto cost = usage.value("estimatedCostUsd");
    if (!cost.isDouble() || !std::isfinite(cost.toDouble()) || cost.toDouble() < 0)
        return {};
    // The SDK reports a cumulative session estimate, never a per-turn charge.
    return QStringLiteral("Session estimate: $%1 USD").arg(cost.toDouble(), 0, 'f', 4);
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
    if (type == "session.provider_changed") {
        title = "Provider changed";
        detail =
            data.value("from").toString() + QStringLiteral(" → ") + data.value("to").toString();
    } else if (type == "session.configuration_changed") {
        title = "Model configuration changed";
        const auto effective = data.value("effective").toObject();
        detail = effective.value("model").toString();
        const auto effort = effective.value("effort").toString();
        if (!effort.isEmpty())
            detail += QStringLiteral(" · ") + effort;
    } else if (type == "conversation.context_changed") {
        title = "Execution worktree changed";
        // The journal intentionally carries revisions, not a machine path.
        detail =
            QStringLiteral("Context revision %1").arg(data.value("context_revision").toInteger());
    } else if (type == "session.forked") {
        title = "Conversation forked";
        detail = "Inherited conversation history";
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
