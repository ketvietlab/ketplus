#include "teams/TeamVerdictLine.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace ketplus::daemon::teams::verdict_line {

Severities count(const QString& reply) {
    Severities found;
    const auto add = [&found](const QChar level) {
        switch (level.toUpper().unicode()) {
        case 'C':
        case 'H':
            ++found.high;
            break;
        case 'M':
            ++found.medium;
            break;
        case 'L':
            ++found.low;
            break;
        case 'N':
            ++found.nit;
            break;
        default:
            break;
        }
    };
    // A finding's id and its severity at the start of a line: "**L1 — Low — file:4:**",
    // "- M2: Medium …", "F3 - nit". Each id counted once, so a finding referred to again
    // later in the reply is not counted twice.
    static const QRegularExpression ids(
        QStringLiteral(R"((?:^|\n)[\s>*_\-•]*\**([A-Z]{1,2}\d{1,3})\**\s*(?:—|–|-|:)\s*\**)"
                       R"((critical|high|medium|low|nit)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    QSet<QString> seen;
    for (auto it = ids.globalMatch(reply); it.hasNext();) {
        const auto match = it.next();
        const QString id = match.captured(1).toUpper();
        if (seen.contains(id))
            continue;
        seen.insert(id);
        add(match.captured(2).at(0));
    }
    if (found.any())
        return found;
    // Without ids: one line per finding that names its severity before a colon.
    static const QRegularExpression levels(
        QStringLiteral(R"(\b(critical|high|medium|low|nit)\b\s*:)"),
        QRegularExpression::CaseInsensitiveOption);
    for (const QString& line : reply.split(QLatin1Char('\n'))) {
        if (line.trimmed().startsWith(QStringLiteral("Verdict"), Qt::CaseInsensitive))
            continue;
        if (const auto match = levels.match(line); match.hasMatch())
            add(match.captured(1).at(0));
    }
    return found;
}

QString counts(const Severities& severities) {
    if (!severities.any())
        return QStringLiteral("severity not stated");
    return QStringLiteral("%1 High, %2 Medium, %3 Low, %4 Nit")
        .arg(severities.high)
        .arg(severities.medium)
        .arg(severities.low)
        .arg(severities.nit);
}

QString text(const Verdict& verdict) {
    QString line = QStringLiteral("Verdict on %1").arg(verdict.taskId);
    if (!verdict.pullRequest.isEmpty())
        line += QStringLiteral(" (PR %1)").arg(verdict.pullRequest);
    line += QStringLiteral(", round %1 of %2 by %3: ")
                .arg(verdict.round)
                .arg(verdict.limit)
                .arg(verdict.reviewer);
    if (!verdict.findings)
        return line + QStringLiteral("no findings. Ready to merge.");
    line += QStringLiteral("findings (%1).").arg(counts(count(verdict.reply)));
    line += verdict.atLimit
                ? QStringLiteral(" The round limit is reached: you decide.")
                : QStringLiteral(" Back to its developer; decide on any finding outside the "
                                 "task's scope.");
    return line;
}

} // namespace ketplus::daemon::teams::verdict_line
