#pragma once

#include "delivery/DeliveryPorts.h"

#include <QString>

#include <optional>

// `journal.json`: the current job's JournalRecord, replaced atomically before each step and
// removed when the lease is closed.
namespace ketplus::runner::app {

class StateDirectory;

class JobJournalFile final : public delivery::DeliveryJournal {
  public:
    explicit JobJournalFile(const StateDirectory& directory) : m_directory(directory) {}

    struct Loaded {
        bool ok = false; // false: unreadable or corrupt, left as it is; see `error`
        std::optional<delivery::JournalRecord> record; // nothing: no job was held
        QString error;
    };
    [[nodiscard]] Loaded load() const;

    [[nodiscard]] bool write(const delivery::JournalRecord& record) override;
    [[nodiscard]] bool clear() override;
    // The reason of the last failed write or clear, naming the file only.
    [[nodiscard]] const QString& error() const { return m_error; }

  private:
    const StateDirectory& m_directory;
    QString m_error;
};

} // namespace ketplus::runner::app
