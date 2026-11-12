#pragma once

#include <QString>

namespace ketplus::daemon::teams {

// T-75 (ADR 0114 §7): the line a review verdict adds to Updates, which the leader reads in
// its next digest, the team page shows and the phone receives.
namespace verdict_line {

// What a review reply says about its findings, by severity.
struct Severities final {
    int high{0};
    int medium{0};
    int low{0};
    int nit{0};
    // The reply named findings by severity, though all of them may be settled.
    bool stated{false};

    [[nodiscard]] bool any() const { return high + medium + low + nit > 0; }
};

// Counts the open findings of a review reply, one per line that starts with a finding's id and
// its severity ("- **H1, High.** …", "- **L1 (Low), file:4:** …", "**M1 — Medium — …**") or
// with its severity alone ("- **Low**, `file:4`: …"). An id is counted once. A finding marked
// fixed ("L1 — Low — fixed", "✅ L2 …") is not counted. A reply without such lines counts the
// lines that name a severity before a colon ("web/cart.ts:4 high: no total."), but not a
// summary of counts ("High: 0, Medium: 1"). Critical counts as High.
[[nodiscard]] Severities count(const QString& reply);

// "0 High, 0 Medium, 6 Low, 3 Nit", or "severity not stated" when the reply named none.
[[nodiscard]] QString counts(const Severities& severities);

struct Verdict final {
    QString taskId;
    QString pullRequest;
    int round{0};
    int limit{0};
    QString reviewer;
    bool findings{false};
    // The leader decides next: the round limit was reached with findings.
    bool atLimit{false};
    QString reply;
};

// "Verdict on T-61 (PR https://…/pull/12), round 1 of 2 by Reviewer: findings (0 High,
// 0 Medium, 6 Low, 3 Nit). Back to its developer; decide on any finding outside the task's
// scope."
[[nodiscard]] QString text(const Verdict& verdict);

} // namespace verdict_line
} // namespace ketplus::daemon::teams
