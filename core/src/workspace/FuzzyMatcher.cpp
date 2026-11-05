#include "workspace/FuzzyMatcher.h"

#include <QtGlobal>

namespace ketplus {
namespace {

constexpr int matchScore = 1;
constexpr int consecutiveBonus = 5;
constexpr int boundaryBonus = 8;
constexpr int fileNameBonus = 3;
constexpr int exactCaseBonus = 1;

bool isSeparator(const QChar character) {
    return character == u'/' || character == u'\\' || character == u'_' || character == u'-' ||
           character == u'.' || character == u' ' || character == u':' || character == u'›';
}

bool isWordBoundary(const QStringView text, const qsizetype index) {
    if (index == 0) {
        return true;
    }
    const QChar previous = text.at(index - 1);
    return isSeparator(previous) || (previous.isLower() && text.at(index).isUpper());
}

qsizetype fileNameStart(const QStringView text) {
    for (qsizetype index = text.size() - 1; index >= 0; --index) {
        if (text.at(index) == u'/' || text.at(index) == u'\\') {
            return index + 1;
        }
    }
    return 0;
}

} // namespace

int fuzzyMatchScore(const QStringView query, const QStringView candidate) {
    if (query.trimmed().isEmpty()) {
        return 0;
    }

    const qsizetype nameStart = fileNameStart(candidate);
    int score = 0;
    qsizetype candidateIndex = 0;
    qsizetype previousMatch = -2;
    for (const QChar queryCharacter : query) {
        if (queryCharacter.isSpace()) {
            continue;
        }
        const QChar needle = queryCharacter.toLower();
        while (candidateIndex < candidate.size() &&
               candidate.at(candidateIndex).toLower() != needle) {
            ++candidateIndex;
        }
        if (candidateIndex >= candidate.size()) {
            return -1;
        }

        score += matchScore;
        if (candidateIndex == previousMatch + 1) {
            score += consecutiveBonus;
        }
        if (isWordBoundary(candidate, candidateIndex)) {
            score += boundaryBonus;
        }
        if (candidateIndex >= nameStart) {
            score += fileNameBonus;
        }
        if (candidate.at(candidateIndex) == queryCharacter) {
            score += exactCaseBonus;
        }
        previousMatch = candidateIndex;
        ++candidateIndex;
    }

    // Prefer shorter candidates when the match quality is otherwise equal.
    score -= static_cast<int>(qMin<qsizetype>(candidate.size(), 200) / 10);
    return qMax(0, score);
}

} // namespace ketplus
