#pragma once

#include "runner/jobs/RemoteJobPorts.h"

#include <QHash>
#include <QSqlDatabase>

namespace ketplus::daemon {
class BackgroundTaskLedger;
}

// Remote job records in the background task registry. Each put writes the record and moves
// its registry task in one transaction; a task must have been created with runs_on, and its
// first record needs the task still queued and approved.
namespace ketplus::daemon::runner::jobs {

class LedgerRecordStore final : public RecordStore {
  public:
    LedgerRecordStore(QSqlDatabase database, BackgroundTaskLedger& ledger);

    [[nodiscard]] bool put(const RemoteJobRecord& record, qint64 expected) override;
    // Every record that still has its task; one that cannot be read is skipped.
    [[nodiscard]] QList<RemoteJobRecord> load() const override;

    // The tasks whose record could not be read at the last load.
    [[nodiscard]] QStringList unreadable() const { return unreadable_; }
    // Tasks created with runs_on that have no record yet; nullopt when they cannot be read.
    [[nodiscard]] std::optional<QStringList> reserved() const;

    // A local user's cleanup acknowledgement for the next put that ends the task from
    // needs_attention; held in memory and used once. `source` is "user" or "runner_removed".
    [[nodiscard]] bool acknowledge(const QString& taskId, const QString& source,
                                   const QString& userClientId);

    // Ends a task whose record could not be read: interrupted, reason record_unreadable, cleanup
    // unverified, and a readable record in its place so no later load finds it again. Answers
    // the job id the row names (empty when none), or nullopt when nothing could be written.
    [[nodiscard]] std::optional<QString> abandonUnreadable(const QString& taskId);

  private:
    struct Acknowledgement {
        QString source;
        QString userClientId;
    };
    bool write(const RemoteJobRecord& record, qint64 expected);

    QSqlDatabase database_;
    BackgroundTaskLedger& ledger_;
    QHash<QString, Acknowledgement> acknowledgements_;
    mutable QStringList unreadable_;
};

} // namespace ketplus::daemon::runner::jobs
