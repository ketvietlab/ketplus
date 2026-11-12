#pragma once

// ADR 0118: what the remote says about a team task's pull request, asked through the GitHub
// command line tool. Free functions, so the session runtime's git worker can run them.
#include "teams/TeamCoordinator.h"

#include <atomic>

namespace ketplus::daemon::team_pull_requests {

// The answer of `gh pr view --json state,mergeCommit`, or an error for an answer it cannot read.
[[nodiscard]] teams::PullRequestFact parse(const QByteArray& json);
struct Limits final {
    int requestMs{15000};
    // The whole batch: what is left when it runs out is not in the answer, asked next time.
    int batchMs{45000};
};
// Asks about each pull request once; `program` is the tool to run, found on the path when
// empty. A request that does not answer in time ends the batch (the network is likely gone),
// and so does `cancelled`; the pull requests not asked are left out of the answer. Never
// touches a checkout.
[[nodiscard]] QHash<QString, teams::PullRequestFact>
ask(const QString& directory, const QStringList& urls, const QString& program = {},
    Limits limits = {}, const std::atomic_bool* cancelled = nullptr);

} // namespace ketplus::daemon::team_pull_requests
