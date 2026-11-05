#include "agent/AgentTranscriptPaging.h"

#include <algorithm>
#include <cmath>

namespace ketplus::transcript_paging {
namespace {
PageCounts& counts() {
    static PageCounts value;
    return value;
}
} // namespace

PageCounts pageCounts() {
    return counts();
}

void setPageCounts(const PageCounts counts_) {
    counts() = {std::max(1, counts_.initial), std::max(1, counts_.continuation), counts_.readAhead,
                counts_.fillBuffer};
}

int adaptiveTurnLimit(const double viewportHeight, const double averageRowHeight,
                      const double rowsPerTurn, const int daemonMaxTurns) {
    const int upper = std::clamp(daemonMaxTurns, 1, maximumTurns);
    const int lower = std::min(minimumTurns, upper);
    if (!(viewportHeight > 0) || !(averageRowHeight > 0) || !(rowsPerTurn > 0))
        return lower;
    const double turnHeight = averageRowHeight * rowsPerTurn;
    const double wanted = std::ceil(viewportsPerPage * viewportHeight / turnHeight);
    if (!std::isfinite(wanted))
        return upper;
    return static_cast<int>(std::clamp(wanted, double(lower), double(upper)));
}

bool batchWantsMore(const int pages, const int pagesWanted, const qsizetype rows,
                    const double viewportHeight, const double heightPerRow) {
    if (pages < pagesWanted)
        return true;
    if (pages >= maximumBatchPages || !(viewportHeight > 0) || !(heightPerRow > 0))
        return false;
    return double(rows) * heightPerRow < batchViewports * viewportHeight;
}

QJsonObject windowParams(const Direction direction, const QString& cursor, qint64 snapshotCursor,
                         const qint64 projectionRevision, const int turnLimit) {
    QJsonObject result{{"turn_limit", std::clamp(turnLimit, 1, maximumTurns)},
                       {"event_limit", eventLimit}};
    if (direction == Direction::Newer) {
        // after_cursor must be a positive event cursor; never send 0.
        if (cursor.toLongLong() > 0)
            result.insert("after_cursor", cursor.toLongLong());
    } else
        result.insert("before_cursor", cursor.toLongLong());
    if (!cursor.isEmpty()) {
        // Trimming a window grown by a live turn resumes older paging at a live
        // event, past the snapshot the window was read with. Every event before
        // it is journaled, so the fence moves up to cover it; the daemon rejects
        // a before_cursor beyond snapshot_cursor + 1 as out of range.
        const qint64 before = cursor.toLongLong();
        if (direction == Direction::Older && before > snapshotCursor + 1)
            snapshotCursor = before - 1;
        result.insert("snapshot_cursor", snapshotCursor);
        result.insert("projection_revision", projectionRevision);
    }
    return result;
}

} // namespace ketplus::transcript_paging
