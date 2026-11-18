#pragma once

#include "runner/jobs/RemoteJobRelay.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

// Relay cancels of remote jobs whose record already ended on this desktop, such as a job whose
// spec was lost when the daemon restarted. A job the Relay never named is submitted again with
// the same key to learn its id. Each cancel is repeated, 1 second doubling to 30, until the Relay
// answers it, no longer knows the job or refuses it for good (a code jobs::submitRefusalReason
// names); the record is never changed by any of it. Errors that can pass, such as the network,
// signing out or the Relay no longer offering runners, are retried at that pace for as long as
// the daemon runs. The cancels are held in memory only: one still pending when the daemon stops
// is not sent again.
namespace ketplus::daemon::runner::remote {

class RelayCancels final {
  public:
    // Runs `work` after `delayMs` on the event loop.
    using Schedule = std::function<void(qint64 delayMs, std::function<void()> work)>;

    static constexpr qint64 firstRetryMs = 1'000;
    static constexpr qint64 longestRetryMs = 30'000;

    RelayCancels(jobs::JobRelay& relay, Schedule schedule);
    ~RelayCancels();
    RelayCancels(const RelayCancels&) = delete;
    RelayCancels& operator=(const RelayCancels&) = delete;

    // `jobId` empty: the job is submitted again under `taskId` with `runsOn` to learn it.
    void cancel(const QString& taskId, const QString& jobId, const QStringList& runsOn);
    [[nodiscard]] bool pending(const QString& taskId) const { return cancels_.contains(taskId); }
    [[nodiscard]] int size() const { return static_cast<int>(cancels_.size()); }

  private:
    struct Cancel {
        QString jobId;
        QStringList runsOn;
        qint64 retryMs = firstRetryMs;
        quint64 generation = 0;
    };

    void attempt(const QString& taskId);
    void named(const QString& taskId, quint64 generation, const jobs::JobOutcome& outcome);
    void answered(const QString& taskId, quint64 generation, const jobs::JobOutcome& outcome);
    void retry(const QString& taskId);

    jobs::JobRelay& relay_;
    Schedule schedule_;
    QHash<QString, Cancel> cancels_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace ketplus::daemon::runner::remote
