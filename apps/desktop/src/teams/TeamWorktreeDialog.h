#pragma once

// ADR 0114 §10 (T-37): the disk under a team's worktrees and the removal of a done task's
// worktree, as the team page shows them. The daemon measures, checks and removes; nothing
// here removes anything without the user's confirmation in the dialog.
#include <QJsonObject>
#include <QString>
#include <QtGlobal>

class QWidget;

namespace ket {
class Dialog;
} // namespace ket

namespace ketplus {

class TeamDirectory;

namespace team_storage {

// Bytes in decimal units with one decimal, as the Finder counts them: "18.4 GB", "640.0 MB".
[[nodiscard]] QString size(qint64 bytes);

// What the page's banner says for team.storage.get's answer: an empty title when the space
// is enough or unknown.
struct Warning final {
    bool stop{false};
    QString title;
    QString message;
};
[[nodiscard]] Warning warning(const QJsonObject& storage);

} // namespace team_storage

// Asks the user to confirm removing `entry` (one of team.storage.get's worktrees), then sends
// team.worktree.remove. A refusal or a failure stays in the dialog, in the daemon's words, and
// the removal cannot be sent again from it; a removal closes it.
[[nodiscard]] ket::Dialog* teamWorktreeRemoval(QWidget* anchor, TeamDirectory* teams,
                                               const QString& teamId, const QJsonObject& entry);

} // namespace ketplus
