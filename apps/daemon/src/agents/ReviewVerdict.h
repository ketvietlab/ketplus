#pragma once

// ADR 0067: the verdict line a reviewer ends its answer with. Shared by the broker's turn
// reviews and a team's review rounds (ADR 0114 §2), so both read the same line.
#include <QRegularExpression>
#include <QString>

namespace ketplus::daemon {

enum class ReviewVerdict { none, findings, noFindings };

// The reviewer's last "Verdict:" line. A reply without one has no verdict: it is not read as
// findings (T-19).
inline ReviewVerdict reviewVerdict(const QString& text) {
    static const QRegularExpression verdict(
        QStringLiteral("^\\W*verdict\\W*:\\s*(no findings|findings)\\b"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    QString last;
    for (auto match = verdict.globalMatch(text); match.hasNext();)
        last = match.next().captured(1).toLower();
    if (last.isEmpty())
        return ReviewVerdict::none;
    return last == QStringLiteral("no findings") ? ReviewVerdict::noFindings
                                                 : ReviewVerdict::findings;
}

// The verdict as the journal, the transcript and the team's history name it.
inline QString verdictName(const ReviewVerdict verdict) {
    switch (verdict) {
    case ReviewVerdict::noFindings:
        return QStringLiteral("no_findings");
    case ReviewVerdict::findings:
        return QStringLiteral("findings");
    case ReviewVerdict::none:
        break;
    }
    return QStringLiteral("no_verdict");
}

} // namespace ketplus::daemon
