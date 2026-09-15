#pragma once

#include <QStringView>

namespace ketplus {

// Scores how well `query` matches `candidate` as a case-insensitive subsequence.
// Returns -1 when it does not match; higher scores are better. Word boundaries,
// camel humps, consecutive runs and matches inside the file name score higher.
[[nodiscard]] int fuzzyMatchScore(QStringView query, QStringView candidate);

} // namespace ketplus
