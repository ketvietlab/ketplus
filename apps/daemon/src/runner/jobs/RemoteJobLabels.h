#pragma once

#include <QStringList>

// The `runs_on` rule of a remote job: 1 to 16 distinct labels, each os:linux, arch:x86_64,
// arch:arm64 or a user label of 1 to 64 characters matching [a-z0-9][a-z0-9._-]*.
namespace ketplus::daemon::runner::jobs {

inline constexpr qsizetype maximumRunsOn = 16;

[[nodiscard]] bool isRunsOnLabel(const QString& label);
// True when `labels` is a valid runs_on; otherwise the start is refused invalid_labels.
[[nodiscard]] bool isValidRunsOn(const QStringList& labels);
// The labels in their one stored order, so the same labels in any order name the same job.
[[nodiscard]] QStringList normalizedRunsOn(QStringList labels);

} // namespace ketplus::daemon::runner::jobs
