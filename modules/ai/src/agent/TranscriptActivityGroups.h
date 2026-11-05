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
//   groupSize     number of rows in the group, set on the leader
//   groupRunning  whether any row in the group is still running, set on every row of the group
//   groupFailed   number of failed rows, set on the leader
[[nodiscard]] bool isGroupableActivity(const QVariantMap& row);
[[nodiscard]] QString activityGroupSummary(const QList<QVariantMap>& rows, qsizetype first,
                                           qsizetype count);
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
