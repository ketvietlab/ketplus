#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace ketplus::agent_message {

// Presentation-only grouping of consecutive activity rows within one turn, summarized the way
// agent tools do ("Ran 3 commands, read 2 files, edited a file +1 -1"). Rows are never removed:
// session indexes, streaming updates and reading anchors keep addressing the same rows, and
// only these roles change.
//   groupKey      stable group identity (expansion state is remembered by it): the messageKey
//                 of the run's first leader, kept while older rows are prepended or the leader
//                 is trimmed away
//   groupLeaderKey  (internal field, not a role) messageKey of the current leader row
//   groupRole     "leader" for the first row, "member" for the rest, empty when ungrouped
//   groupSummary  summary text, set on the leader
//   groupLatest   the title of the group's last row, set on the leader: a running group's
//                 heading names the step it is on (T-91)
//   groupSize     number of rows in the group, set on the leader
//   groupRunning  whether any row in the group is still running, set on every row of the group
//   groupFailed   number of failed rows, set on the leader
//   groupTail     whether no row follows the group, set on the leader (and on a daemon group
//                 row): while the turn runs, such a group has not ended even when none of its
//                 rows is running, between two steps (T-91)
[[nodiscard]] bool isGroupableActivity(const QVariantMap& row);
// What a group's steps did, in first-seen order, so a local run and a daemon summary read
// the same. Family ids: command, read, list, edit, search (the web), codeSearch, fileSearch,
// searched (any search, from older daemon groups), webFetch, toolsLoaded, skill, taskStopped,
// server (tool calls, named by `servers`), subagent, wait, thought, image, generated and tool.
struct ActivityTally final {
    struct Family final {
        QString id;
        int count{0};
    };
    QList<Family> families;
    // Tool servers in first-seen order, with the steps each took.
    QList<Family> servers;
    int subagents{0};
    int added{0};
    int removed{0};
    bool statsKnown{false};
    int failed{0};
    void add(const QString& id, int amount);
    void addServer(const QString& server, int steps);
};
// "Ran 3 commands, used ketplus_browser 4 times, edited a file +1 -1 (1 failed)". Thoughts
// are not counted unless the group has nothing else.
[[nodiscard]] QString activityTallySummary(const ActivityTally& tally);
[[nodiscard]] QString activityGroupSummary(const QList<QVariantMap>& rows, qsizetype first,
                                           qsizetype count);
// The heading of a running group: the title its last row shows, on one line.
[[nodiscard]] QString activityGroupLatest(const QVariantMap& row);
// Recomputes group roles for every row and returns the indexes whose group roles changed.
QList<int> assignActivityGroups(QList<QVariantMap>& rows);
// Incremental form: recomputes only the runs touching rows [first, last] (widened to the
// enclosing groupable stretches and their neighbours) and returns the changed indexes.
QList<int> assignActivityGroups(QList<QVariantMap>& rows, int first, int last);
// Name of the internal leader field (not a model role; it changes with prepends without
// requiring a dataChanged).
[[nodiscard]] const QString& activityGroupLeaderKeyName();
// Role names written by assignActivityGroups().
[[nodiscard]] const QStringList& activityGroupRoleNames();

} // namespace ketplus::agent_message
