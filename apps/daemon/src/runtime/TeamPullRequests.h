#pragma once

// ADR 0118: what the remote says about a team task's pull request, asked through the GitHub
// command line tool. Free functions, so the session runtime's git worker can run them.
#include "teams/TeamCoordinator.h"

namespace ketplus::daemon::team_pull_requests {

// The answer of `gh pr view --json state,mergeCommit`, or an error for an answer it cannot read.
[[nodiscard]] teams::PullRequestFact parse(const QByteArray& json);
// Asks about each pull request once, with a timeout; `program` is the tool to run, found on
// the path when empty. Never touches a checkout.
[[nodiscard]] QHash<QString, teams::PullRequestFact>
ask(const QString& directory, const QStringList& urls, const QString& program = {});

} // namespace ketplus::daemon::team_pull_requests
