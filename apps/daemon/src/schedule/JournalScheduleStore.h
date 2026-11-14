#pragma once

#include "schedule/ScheduleRunner.h"

namespace ketplus::daemon {

class LocalJournal;

// ScheduleStore over the journal tables from ADR 0126. The runner never opens SQLite itself.
class JournalScheduleStore final : public schedule::ScheduleStore {
  public:
    explicit JournalScheduleStore(LocalJournal* journal);

    [[nodiscard]] QList<::ketplus::schedule::Schedule> schedules() const override;
    [[nodiscard]] std::optional<::ketplus::schedule::Schedule>
    schedule(const QString& id) const override;
    bool save(const ::ketplus::schedule::Schedule& schedule) override;
    ::ketplus::schedule::Run insertRun(::ketplus::schedule::Run run) override;
    bool saveRun(const ::ketplus::schedule::Run& run) override;
    [[nodiscard]] std::optional<::ketplus::schedule::Run> run(const QString& id) const override;
    [[nodiscard]] std::optional<::ketplus::schedule::Run>
    queuedRun(const QString& scheduleId) const override;

  private:
    LocalJournal* journal_;
};

} // namespace ketplus::daemon
