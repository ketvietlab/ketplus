#pragma once

#include <QJsonArray>
#include <QString>

namespace ketplus::daemon::agent_mcp {

// ADR 0114: the team_* tools, listed only for a member of a team.
[[nodiscard]] QJsonArray teamTools();
// The daemon operation of a team tool, or empty when `tool` is not one.
[[nodiscard]] QString teamOperationFor(const QString& tool);

} // namespace ketplus::daemon::agent_mcp
