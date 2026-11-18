#pragma once

#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace ketplus::daemon {
struct BackgroundTaskIntent;
}
namespace ketplus::daemon::runner::remote {
class RemoteJobs;
}

// Where the background task runtime meets the remote jobs: the checks before a task with
// runs_on exists, its submit once approved, and its records in replies. A task with runs_on
// never runs on this machine.
namespace ketplus::daemon::runner::entry {

class RemoteJobEntry final {
  public:
    struct Ports {
        // Ends a reserved task that never got a record: failed with `code`, never launched.
        // Ignored by the runtime when the task already ended.
        std::function<void(const QString& taskId, const QString& code)> failReserved;
        // A remote record was stored: the runtime reads the task again and publishes it.
        std::function<void(const QString& taskId)> changed;
    };

    explicit RemoteJobEntry(remote::RemoteJobs& jobs);
    ~RemoteJobEntry();
    RemoteJobEntry(const RemoteJobEntry&) = delete;
    RemoteJobEntry& operator=(const RemoteJobEntry&) = delete;

    void setPorts(Ports ports) { ports_ = std::move(ports); }

    // A failure reply when a task with runs_on cannot be created now: runner_unavailable,
    // git_unsupported, not_a_worktree, resource_exhausted or storage_error. Empty when it can,
    // and for a local task. Creates nothing.
    [[nodiscard]] QJsonObject admit(const BackgroundTaskIntent& intent,
                                    const QJsonObject& execution) const;

    // Once the task was approved: freezes its worktree and submits it, once. A submit that ends
    // before a record exists ends the reserved task through Ports::failReserved.
    void submit(const QString& taskId, const QJsonObject& execution, qint64 timeoutMs,
                const QStringList& runsOn);

    // nullopt when the task has no remote record (a local task, or one still reserved: the
    // runtime ends it as a queued task). Otherwise the stop's code, empty when it was taken.
    [[nodiscard]] std::optional<QString> stop(const QString& taskId);
    // nullopt when the task has no remote record; otherwise the acknowledgement's code.
    [[nodiscard]] std::optional<QString> resolveCleanup(const QString& taskId,
                                                        const QString& userClientId);

    // `placement` and `runner` added to the task or tasks of a get or list reply.
    [[nodiscard]] QJsonObject withPlacement(QJsonObject reply) const;
    [[nodiscard]] bool hasRecord(const QString& taskId) const;

  private:
    [[nodiscard]] QJsonObject placed(QJsonObject task) const;

    remote::RemoteJobs& jobs_;
    Ports ports_;
    QSet<QString> submitted_; // tasks this entry submitted; a task is submitted once
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

// What the user does about a remote job left in needs_attention.
[[nodiscard]] QString remoteManualGuidance();

} // namespace ketplus::daemon::runner::entry
