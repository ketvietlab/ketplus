#pragma once

#include "teams/Team.h"

namespace ketplus::daemon::teams::completion {
[[nodiscard]] std::optional<TaskAcceptance> fromJson(const QJsonValue& value);
[[nodiscard]] QJsonObject toJson(const TaskAcceptance& acceptance);
[[nodiscard]] QString context(const Team& team, const Task& task);
[[nodiscard]] QString repositoryId(QString url);
void seal(const Team& team, Task& task, const QString& source, const QString& actor,
          const QString& repository = {});
[[nodiscard]] bool valid(const Team& team, const Task& task);
[[nodiscard]] bool canFinish(const Team& team, const Task& task);
} // namespace ketplus::daemon::teams::completion
