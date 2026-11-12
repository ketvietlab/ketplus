#pragma once

// T-36: what team_tasks answers an agent. The list is one compact row per task, so it stays
// small however long the tasks' descriptions and histories grow; one task is read with a
// page of its history, and one entry with its whole text. The desktop and paired devices read
// tasks through summaryJson and taskPageJson, which this does not change.
#include "teams/Team.h"

#include <QJsonArray>
#include <QJsonObject>

#include <functional>
#include <optional>

namespace ketplus::daemon::teams {

// The newest entries one task read returns when no page is asked for.
inline constexpr int agentHistoryPage = 10;
// The most entries one page returns.
inline constexpr int agentHistoryMaximum = 50;
// A text in a page longer than this is cut; the cut says how to read the whole entry.
inline constexpr qsizetype agentEntryText = maximumHistoryText;

struct AgentTaskFilter final {
    // Only the tasks that are not done.
    bool open{false};
    // Only the tasks the caller owns or reviews.
    QString mine;
};

// A member's conversation number and name, by session id; empty for nobody.
struct AgentMember final {
    QString number;
    QString name;
};
using MemberLookup = std::function<AgentMember(const QString& sessionId)>;

// id, title, state, round, owner {number, name} or null, lane, depends_on, pull_request, and
// for a blocked task its question.
[[nodiscard]] QJsonObject agentTaskRow(const Task& task, const AgentMember& owner);
[[nodiscard]] QJsonArray agentTaskRows(const Team& team, const AgentTaskFilter& filter,
                                       const MemberLookup& members);
// The task with its description, criteria and `limit` history entries from `start`, or the
// newest ones when `start` is negative; "history_total" and "history_start" say where the
// page is. Each entry carries its "entry" index.
[[nodiscard]] QJsonObject agentTaskPage(const Task& task, qsizetype start = -1,
                                        int limit = agentHistoryPage);
// One entry, uncut: `wholeText` is the journal's copy of a text the document keeps cut.
[[nodiscard]] QJsonObject agentTaskEntry(const Task& task, qsizetype index,
                                         const std::optional<QString>& wholeText);

} // namespace ketplus::daemon::teams
