#include "runner/peer/OutputIntake.h"

namespace ketplus::daemon::runner::peer {

namespace {

OutputStep stepOf(const OutputStep::Kind kind) {
    OutputStep step;
    step.kind = kind;
    return step;
}

} // namespace

OutputIntake::OutputIntake(OutputSpool& spool) : spool_(spool), acked_(spool.size()) {}

OutputStep OutputIntake::page(const qint64 offset, const QByteArray& data) {
    const qint64 size = spool_.size();
    if (offset > size)
        return ahead();
    const qint64 end = offset + data.size();
    if (end <= size)
        return {};
    if (!spool_.append(data.mid(size - offset)))
        return stepOf(OutputStep::Kind::Failed);
    return written(stepOf(OutputStep::Kind::Written));
}

OutputStep OutputIntake::gap(const qint64 fromOffset, const qint64 toOffset) {
    const qint64 size = spool_.size();
    if (fromOffset > size)
        return ahead();
    if (toOffset <= size)
        return {};
    if (!spool_.markGap(toOffset))
        return stepOf(OutputStep::Kind::Failed);
    return written(stepOf(OutputStep::Kind::Written));
}

OutputStep OutputIntake::written(OutputStep step) {
    const qint64 size = spool_.size();
    if (size - acked_ >= outputAckBytes) {
        acked_ = size;
        step.ackAt = size;
    } else if (size > acked_) {
        step.armAckTimer = true;
    }
    return step;
}

OutputStep OutputIntake::ahead() {
    OutputStep step = stepOf(OutputStep::Kind::Ahead);
    const qint64 size = spool_.size();
    if (resumedAt_ != size) {
        resumedAt_ = size;
        step.resumeFrom = size;
    }
    return step;
}

std::optional<qint64> OutputIntake::ackDue() {
    const qint64 size = spool_.size();
    if (size <= acked_)
        return std::nullopt;
    acked_ = size;
    return size;
}

qint64 OutputIntake::resumeFromSpool() {
    resumedAt_ = spool_.size();
    return resumedAt_;
}

void OutputIntake::reconnected() {
    acked_ = spool_.size();
    resumedAt_ = -1;
}

} // namespace ketplus::daemon::runner::peer
