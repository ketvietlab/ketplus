#pragma once

#include <QJsonObject>
#include <QString>

// Pure, unit-testable transcript window paging parameters (daemon session.transcript_window).
namespace ketplus::transcript_paging {

inline constexpr int minimumTurns = 3;
inline constexpr int maximumTurns = 20;
inline constexpr int legacyMaximumTurns = 3;
inline constexpr int eventLimit = 128;
// Daemon pages read together before the model is touched: the first window of a
// conversation, and every lazy load after it.
struct PageCounts final {
    int initial{1};
    int continuation{1};
    // Whether pages beyond the loaded window are read ahead in the background.
    bool readAhead{true};
    // Whether a lazy load reads past `continuation` to fill batchViewports.
    bool fillBuffer{false};
};
[[nodiscard]] PageCounts pageCounts();
// Tests that walk the window one page at a time set both to 1, without read-ahead
// or buffer filling.
void setPageCounts(PageCounts counts);
// A page should fill about this many viewport heights.
inline constexpr double viewportsPerPage = 3.0;

// A lazy-load batch keeps reading pages until its rows are estimated to fill this
// many viewports, so a fast scroll does not outrun pages made of a few tall rows
// or of one long turn split across many event-limited pages.
inline constexpr double batchViewports = 6.0;
inline constexpr int maximumBatchPages = 40;

enum class Direction { Older, Newer };

// Whether a lazy-load batch that has read `pages` pages holding `rows` rows reads
// another. `pagesWanted` pages are always read; unknown geometry stops there.
[[nodiscard]] bool batchWantsMore(int pages, int pagesWanted, qsizetype rows, double viewportHeight,
                                  double heightPerRow);

// Estimated turns needed to fill viewportsPerPage viewports. Unknown geometry (<= 0) yields
// minimumTurns. The result is clamped to [minimumTurns, min(maximumTurns, daemonMaxTurns)].
[[nodiscard]] int adaptiveTurnLimit(double viewportHeight, double averageRowHeight,
                                    double rowsPerTurn, int daemonMaxTurns);

// Initial window when cursor is empty (Older only); otherwise a fenced continuation.
[[nodiscard]] QJsonObject windowParams(Direction direction, const QString& cursor,
                                       qint64 snapshotCursor, qint64 projectionRevision,
                                       int turnLimit = minimumTurns);

} // namespace ketplus::transcript_paging
