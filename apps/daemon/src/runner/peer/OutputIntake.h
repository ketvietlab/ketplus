#pragma once

#include "runner/peer/PeerPorts.h"

#include <optional>

// Takes a job's `job.output` and `job.output.gap` pages into its spool at their offsets and
// decides when `job.output.ack` and `job.output.resume` are due. Pure: the session sends.
namespace ketplus::daemon::runner::peer {

inline constexpr qint64 outputAckBytes = 256 * 1024;
inline constexpr qint64 outputAckMs = 1000;

struct OutputStep {
    enum class Kind {
        Written,   // new bytes are durable (or a gap was recorded)
        Duplicate, // everything was already in the spool
        Ahead,     // starts beyond the spool: ask again from the spool's size
        Failed,    // the spool could not write
    };
    Kind kind = Kind::Duplicate;
    // Set when `job.output.resume` from this offset is due (once per missing range).
    std::optional<qint64> resumeFrom;
    // Set when `job.output.ack` with this offset is due now.
    std::optional<qint64> ackAt;
    // True when the 1-second ack timer must be armed.
    bool armAckTimer = false;
};

class OutputIntake {
  public:
    explicit OutputIntake(OutputSpool& spool);

    [[nodiscard]] OutputStep page(qint64 offset, const QByteArray& data);
    [[nodiscard]] OutputStep gap(qint64 fromOffset, qint64 toOffset);
    // The ack timer fired: the offset to acknowledge, if any byte is still unacknowledged.
    [[nodiscard]] std::optional<qint64> ackDue();
    // A new connection, resumed from size(): that offset counts as acknowledged.
    void reconnected();
    // A `job.output.resume` from size() goes out now: the offset it asks from.
    [[nodiscard]] qint64 resumeFromSpool();

    [[nodiscard]] qint64 size() const { return spool_.size(); }
    [[nodiscard]] qint64 acknowledged() const { return acked_; }

  private:
    [[nodiscard]] OutputStep written(OutputStep step);
    [[nodiscard]] OutputStep ahead();

    OutputSpool& spool_;
    qint64 acked_ = 0;      // the last offset acknowledged to the runner
    qint64 resumedAt_ = -1; // the spool size a resume was last asked from
};

} // namespace ketplus::daemon::runner::peer
