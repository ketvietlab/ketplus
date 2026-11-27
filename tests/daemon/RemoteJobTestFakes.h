#pragma once

#include "runner/jobs/RemoteJobCoordinator.h"

#include <QTest>

#include <map>

// In-memory ports for the remote job coordinator: a Relay whose answers the test gives, a clock
// the test moves, runner pins and the peer session's cancel requests.
namespace remote_job_test {

using namespace ketplus::daemon::runner;
using namespace ketplus::daemon::runner::jobs;

inline RelayJob relayJob(const QString& state, const std::optional<QString>& runnerId = {},
                         const int attempt = 0, const std::optional<QString>& reason = {}) {
    RelayJob job;
    job.jobId = QStringLiteral("job-1");
    job.state = state;
    job.attempt = attempt;
    job.runnerId = runnerId;
    job.queuedAt = QDateTime(QDate(2026, 11, 18), QTime(9, 0), QTimeZone::UTC);
    job.expiresAt = job.queuedAt.addDays(1);
    job.reason = reason;
    return job;
}

class FakeRelay final : public JobRelay {
  public:
    struct Call {
        QString operation;
        QString id; // submitKey or jobId
        QString reason;
        QStringList runsOn;
        JobCallback callback;
    };

    [[nodiscard]] bool available() const override { return offered; }
    void submit(const QString& key, const QStringList& runsOn, JobCallback callback) override {
        calls.append({QStringLiteral("submit"), key, {}, runsOn, std::move(callback)});
        log.append(calls.last());
    }
    void get(const QString& jobId, JobCallback callback) override {
        calls.append({QStringLiteral("get"), jobId, {}, {}, std::move(callback)});
        log.append(calls.last());
    }
    void cancel(const QString& jobId, const QString& reason, JobCallback callback) override {
        calls.append({QStringLiteral("cancel"), jobId, reason, {}, std::move(callback)});
        log.append(calls.last());
    }

    [[nodiscard]] int pending(const QString& operation) const {
        return static_cast<int>(std::count_if(
            calls.cbegin(), calls.cend(), [&](const Call& c) { return c.operation == operation; }));
    }
    [[nodiscard]] int sent(const QString& operation) const {
        return static_cast<int>(std::count_if(
            log.cbegin(), log.cend(), [&](const Call& c) { return c.operation == operation; }));
    }
    // Answers the oldest pending call of `operation`; false when there is none.
    bool answer(const QString& operation, const JobOutcome& outcome) {
        for (qsizetype index = 0; index < calls.size(); ++index) {
            if (calls.at(index).operation != operation)
                continue;
            const Call call = calls.takeAt(index);
            call.callback(outcome);
            return true;
        }
        return false;
    }
    bool answer(const QString& operation, const RelayJob& job) {
        return answer(operation, JobOutcome{job, {}});
    }
    bool refuse(const QString& operation, const QString& code) {
        return answer(operation, JobOutcome{std::nullopt, runnerFailure(code)});
    }

    bool offered = true;
    QList<Call> calls; // pending
    QList<Call> log;   // every call sent
};

class FakeClock final : public JobClock {
  public:
    [[nodiscard]] QDateTime now() const override { return time; }
    void arm(const QString& taskId, const JobTimer timer, const qint64 delayMs) override {
        due[{taskId, static_cast<int>(timer)}] = time.toMSecsSinceEpoch() + delayMs;
        armed.append({static_cast<int>(timer), delayMs});
    }
    void disarm(const QString& taskId, const JobTimer timer) override {
        due.erase({taskId, static_cast<int>(timer)});
    }
    [[nodiscard]] bool isArmed(const QString& taskId, const JobTimer timer) const {
        return due.count({taskId, static_cast<int>(timer)}) > 0;
    }
    // Moves time on and fires each timer that falls due, in order.
    void advance(const qint64 ms) {
        const qint64 end = time.toMSecsSinceEpoch() + ms;
        for (;;) {
            auto next = due.cend();
            for (auto it = due.cbegin(); it != due.cend(); ++it)
                if (it->second <= end && (next == due.cend() || it->second < next->second))
                    next = it;
            if (next == due.cend())
                break;
            const auto [key, when] = *next;
            due.erase(next);
            time = QDateTime::fromMSecsSinceEpoch(when, QTimeZone::UTC);
            target->timerFired(key.first, static_cast<JobTimer>(key.second));
        }
        time = QDateTime::fromMSecsSinceEpoch(end, QTimeZone::UTC);
    }

    QDateTime time{QDate(2026, 11, 18), QTime(9, 0), QTimeZone::UTC};
    std::map<std::pair<QString, int>, qint64> due;
    QList<std::pair<int, qint64>> armed;
    RemoteJobCoordinator* target = nullptr;
};

class FakeTrust final : public RunnerTrust {
  public:
    [[nodiscard]] std::optional<QString> pinnedName(const QString& runnerId) const override {
        const auto found = pins.constFind(runnerId);
        return found == pins.cend() ? std::nullopt : std::optional(*found);
    }
    QHash<QString, QString> pins{{QStringLiteral("runner-a"), QStringLiteral("build-a")},
                                 {QStringLiteral("runner-b"), QStringLiteral("build-b")}};
};

class FakePeers final : public JobPeers {
  public:
    void cancel(const QString& taskId, const QString& runnerId) override {
        cancels.append({taskId, runnerId});
    }
    QList<std::pair<QString, QString>> cancels;
};

inline const QString task = QStringLiteral("task-1");
inline const QString job = QStringLiteral("job-1");
inline const QString runnerA = QStringLiteral("runner-a");
inline const QString runnerB = QStringLiteral("runner-b");

inline JobResult jobResult(const QString& state, const std::optional<qint64> exitCode = {},
                           const QString& reason = {},
                           const QString& cleanupStatus = QStringLiteral("verified")) {
    return JobResult{state, exitCode, std::nullopt, reason, cleanupStatus};
}

// The coordinator on fakes, with helpers that take a record to each placement.
struct Harness {
    FakeRelay relay;
    MemoryRecordStore store;
    FakeClock clock;
    FakeTrust trust;
    FakePeers peers;
    std::unique_ptr<RemoteJobCoordinator> jobs;
    int specs = 0; // dispatch decisions that let a spec go

    Harness() { reopen(); }
    // A new coordinator on the same store, as after a daemon restart (not yet restored).
    void reopen() {
        relay.calls.clear();
        jobs = std::make_unique<RemoteJobCoordinator>(relay, store, clock, trust, peers);
        clock.due.clear();
        clock.target = jobs.get();
    }
    [[nodiscard]] RemoteJobRecord record(const QString& id = task) const {
        return jobs->record(id).value_or(RemoteJobRecord{});
    }
    JobDecision dispatch(const QString& runnerId) {
        const JobDecision decision = jobs->dispatch(job, runnerId);
        specs += decision.ok() ? 1 : 0;
        return decision;
    }
    bool start(const qint64 timeoutMs = 0) {
        return jobs->start(task, {QStringLiteral("os:linux")}, timeoutMs).ok();
    }
    bool queued(const qint64 timeoutMs = 0) {
        return start(timeoutMs) && relay.answer(QStringLiteral("submit"), relayJob("queued"));
    }
    bool assigned(const QString& runnerId = runnerA, const qint64 timeoutMs = 0) {
        return queued(timeoutMs) && jobs->offer(job, runnerId, 1).ok();
    }
    bool sent(const QString& runnerId = runnerA, const qint64 timeoutMs = 0) {
        return assigned(runnerId, timeoutMs) && dispatch(runnerId).ok();
    }
    bool syncing(const qint64 timeoutMs = 0) {
        if (!sent(runnerA, timeoutMs))
            return false;
        jobs->accepted(job, runnerA);
        return record().placement == placement::syncing;
    }
    bool running(const qint64 timeoutMs = 0) {
        if (!syncing(timeoutMs))
            return false;
        jobs->started(job, runnerA);
        return record().state == state::running;
    }
};

} // namespace remote_job_test
