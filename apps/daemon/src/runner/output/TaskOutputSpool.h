#pragma once

#include "runner/peer/PeerPorts.h"
#include "storage/BackgroundTaskOutput.h"

#include <QSqlDatabase>
#include <QString>

#include <memory>

// The output spool of one remote job, kept in the task output store so it reads like the output
// of a local job.
namespace ketplus::daemon::runner::output {

class TaskOutputSpool final : public peer::OutputSpool {
  public:
    // Nothing when the task's offset cannot be read from the store.
    [[nodiscard]] static std::unique_ptr<TaskOutputSpool> open(QSqlDatabase database,
                                                               const QString& taskId);

    // The offset held in memory; changes only after a successful append or gap.
    [[nodiscard]] qint64 size() const override { return size_; }
    // Written only at size(); false, with nothing changed, when the store moved or failed.
    [[nodiscard]] bool append(const QByteArray& bytes) override;
    [[nodiscard]] bool markGap(qint64 toOffset) override;

  private:
    TaskOutputSpool(QSqlDatabase database, QString taskId, qint64 size);

    BackgroundTaskOutput output_;
    QString taskId_;
    qint64 size_ = 0;
};

} // namespace ketplus::daemon::runner::output
