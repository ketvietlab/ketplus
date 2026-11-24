#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>

namespace ketplus::daemon {

// What keeps an owner's ended turn open, over its whole registry rather than the bounded
// 16-row task projection (ADR 0127): live_jobs counts the jobs whose end wakes the owner or
// that need the user, queued_jobs those not started, attention_jobs those in
// needs_attention, and job_title names one, a job needing attention first. A job stopping
// toward cancellation is left out: its completion wake is suppressed. `managed` are the
// owner's nonterminal managed jobs; `legacy` the provider-reported task rows. With nothing
// live it is just {live_jobs: 0}.
[[nodiscard]] QJsonObject backgroundTaskSummary(const QList<QJsonObject>& managed,
                                                const QJsonArray& legacy);

} // namespace ketplus::daemon
