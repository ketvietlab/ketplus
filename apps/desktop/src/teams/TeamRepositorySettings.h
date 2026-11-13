#pragma once

// T-90 (ADR 0121): the team's Repository settings. The user moves a team to another
// repository's main checkout and integration branch; the daemon checks both (a Git repository
// whose origin has the branch), records the change with its reason in Updates, and keeps every
// started task in the repository it began in.
#include <QJsonObject>

class QWidget;

namespace ket {
class Dialog;
} // namespace ket

namespace ketplus {

class TeamDirectory;

// Sends team.update with the repository, the integration branch and the reason, shows the
// daemon's refusal in its words, and closes once the daemon took the change.
[[nodiscard]] ket::Dialog* teamRepositorySettings(QWidget* anchor, TeamDirectory* teams,
                                                  const QJsonObject& team);

} // namespace ketplus
