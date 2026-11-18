#pragma once

#include "runner/entry/RemoteGuidance.h"
#include "runner/entry/RemoteJobPort.h"

#include <QJsonObject>
#include <QSet>
#include <QString>

#include <memory>
#include <optional>

namespace ketplus::daemon::runner::remote {
class RemoteJobs;
}

// Where the background task runtime meets the remote jobs: the checks before a task with
// runs_on exists, its submit once approved, and its records in replies.
namespace ketplus::daemon::runner::entry {

class RemoteJobEntry final : public RemoteJobPort {
  public:
    explicit RemoteJobEntry(remote::RemoteJobs& jobs);
    ~RemoteJobEntry() override;
    RemoteJobEntry(const RemoteJobEntry&) = delete;
    RemoteJobEntry& operator=(const RemoteJobEntry&) = delete;

    void setPorts(Ports ports) override { ports_ = std::move(ports); }

    // invalid_request when a runner cannot take the execution, then runner_unavailable,
    // git_unsupported, not_a_worktree, resource_exhausted or storage_error.
    [[nodiscard]] QJsonObject admit(const BackgroundTaskIntent& intent,
                                    const QJsonObject& execution) const override;
    // The labels and timeout are those the reserved task was created with.
    void submit(const QString& taskId, const QJsonObject& execution) override;
    [[nodiscard]] std::optional<QString> stop(const QString& taskId) override;
    [[nodiscard]] std::optional<QString> resolveCleanup(const QString& taskId,
                                                        const QString& userClientId) override;
    [[nodiscard]] QJsonObject withPlacement(QJsonObject reply) const override;
    [[nodiscard]] bool hasRecord(const QString& taskId) const override;

  private:
    [[nodiscard]] QJsonObject placed(QJsonObject task) const;
    void failReserved(const QString& taskId, const QString& code) const;

    remote::RemoteJobs& jobs_;
    Ports ports_;
    QSet<QString> submitted_; // tasks this entry submitted; a task is submitted once
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace ketplus::daemon::runner::entry
