#include "teams/TeamVerdictLine.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace ketplus::daemon::teams::verdict_line {

Severities count(const QString& reply) {
    Severities found;
    const auto add = [&found](const QString& level) {
        switch (level.at(0).toUpper().unicode()) {
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
    constexpr auto insensitive = QRegularExpression::CaseInsensitiveOption;
    // A line may start with a list mark, a quote, emphasis, or the reviewer's mark that a
    // finding is fixed (✅) or still open (❌). A finding's id, then its severity:
    // "- **H1, High.** `a.cpp:3`", "- **L1 (Low), a.cpp:9:**", "**M1 — Medium — a.cpp:1:**",
    // "- N2: Nit", "[L3] low".
    static const QRegularExpression withId(
        QStringLiteral(R"(^([\s>*_\-•+✅❌]*)[*_\[]*([A-Z]{1,2}\d{1,3})[*_\]]*\s*)"
                       R"((?:[—–\-:,(]\s*)?[*_]*\s*(critical|high|medium|low|nit)\b(.*)$)"),
        insensitive);
    // A finding without an id that starts with its severity: "- **Low**, `a.cpp:4`: …",
    // "High: …", "Nit — …".
    static const QRegularExpression leading(
        QStringLiteral(R"(^([\s>*_\-•+✅❌]*)(critical|high|medium|low|nit)\b[*_]*)"
                       R"((?:\s*[,.:;(]|\s+[—–\-]|\s*$)(.*)$)"),
        insensitive);
    // A finding that names its severity before a colon: "web/cart.ts:4 high: no total." A
    // count after the colon ("High: 0, Medium: 1") is a summary, not a finding.
    static const QRegularExpression named(
        QStringLiteral(R"(\b(critical|high|medium|low|nit)\b\s*:(?!\s*\d)(.*)$)"), insensitive);
    // Round 2 lists the earlier findings again with what became of them: "L1 — Low — fixed".
    static const QRegularExpression settled(
        QStringLiteral(
            R"(^[\s.,:;—–\-)*_\]]*✅?\s*(fixed|resolved|addressed|verified|đã sửa)\b|^[^\w]*✅)"),
        insensitive);
    const auto open = [](const QString& start, const QString& rest) {
        return !start.contains(QChar(0x2705)) && !settled.match(rest).hasMatch();
    };
    const QStringList lines = reply.split(QLatin1Char('\n'));
    QSet<QString> seen;
    for (const QString& line : lines) {
        if (line.trimmed().startsWith(QStringLiteral("Verdict"), Qt::CaseInsensitive))
            continue;
        if (const auto match = withId.match(line); match.hasMatch()) {
            // Each id once: a finding referred to again later is not counted twice.
            const QString id = match.captured(2).toUpper();
            if (seen.contains(id))
                continue;
            seen.insert(id);
            if (open(match.captured(1), match.captured(4)))
                add(match.captured(3));
        } else if (const auto first = leading.match(line); first.hasMatch()) {
            seen.insert(QString());
            if (open(first.captured(1), first.captured(3)))
                add(first.captured(2));
        }
    }
    if (!seen.isEmpty()) {
        found.stated = true;
        return found;
    }
    for (const QString& line : lines) {
        if (line.trimmed().startsWith(QStringLiteral("Verdict"), Qt::CaseInsensitive))
            continue;
        const auto match = named.match(line);
        if (!match.hasMatch())
            continue;
        found.stated = true;
        if (open({}, match.captured(2)))
            add(match.captured(1));
    }
    return found;
}

QString counts(const Severities& severities) {
    if (!severities.stated)
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
