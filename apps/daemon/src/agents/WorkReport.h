#pragma once

#include <QJsonObject>
#include <QString>

namespace ketplus::daemon::work_report {

// ADR 0106: a conversation's work report is one ledger of items with stable keys. The agent
// reports items through the work_report tool; the daemon merges them by key and the app counts.
inline constexpr qsizetype maximumItemsPerCall = 50;
inline constexpr qsizetype maximumItems = 200;
inline constexpr qsizetype maximumTitle = 200;
inline constexpr qsizetype maximumGroup = 80;
inline constexpr qsizetype maximumEvidence = 1000;
inline constexpr qsizetype maximumRemaining = 600;
inline constexpr qsizetype maximumAction = 300;
// The encoded ledger. Its event, and the turn.completed that repeats it, stay well under the
// journal's event limit (protocol::maximumEventPayloadBytes).
inline constexpr qsizetype maximumLedgerBytes = 160 * 1024;

struct Merge final {
    // The merged ledger, or empty with `error` set when the call is rejected as a whole.
    QJsonObject ledger;
    QString error;
    int added{0};
    int updated{0};
};

// Merges one work_report call into `current` (empty for a conversation's first report) during
// turn `turn`:
// - an item upserts by key;
// - items the call does not mention stay as they were;
// - an item leaves the open set only as done or cancelled, and closing it drops its next step
//   and remaining work unless the same call reports them.
// Text is redacted and capped before it is stored. A done item needs evidence or a verified
// check. Over `maximumItems` or `maximumLedgerBytes`, the oldest closed items leave the ledger
// first (the journal's earlier snapshots keep them); a ledger still over is rejected.
[[nodiscard]] Merge merge(const QJsonObject& current, const QJsonObject& call, qint64 turn);

// The ledger for the cloud: every text field redacted again and capped.
[[nodiscard]] QJsonObject cloudProjection(const QJsonObject& ledger);

// The tool's answer: the counts the app shows, so the agent can check its report.
[[nodiscard]] QJsonObject summary(const Merge& merge);

} // namespace ketplus::daemon::work_report
