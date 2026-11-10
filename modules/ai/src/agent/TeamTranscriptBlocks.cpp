#include "agent/TeamTranscriptBlocks.h"

#include <QRegularExpression>
#include <QStringList>

namespace ketplus::team_blocks {
namespace {

QString stateLabel(const QString& state) {
    static const QHash<QString, QString> labels{
        {QStringLiteral("todo"), QStringLiteral("To do")},
        {QStringLiteral("in_progress"), QStringLiteral("In progress")},
        {QStringLiteral("in_review"), QStringLiteral("In review")},
        {QStringLiteral("ready"), QStringLiteral("Ready to merge")},
        {QStringLiteral("blocked"), QStringLiteral("Blocked")},
        {QStringLiteral("done"), QStringLiteral("Done")}};
    return labels.value(state, state);
}

QString valueAfter(const QStringList& lines, const QString& prefix) {
    for (const QString& line : lines)
        if (line.startsWith(prefix))
            return line.mid(prefix.size()).trimmed();
    return {};
}

// "1. [x] text" (an assignment) or "1. text" (a review request).
QVariantList criteriaOf(const QStringList& lines) {
    static const QRegularExpression checked(QStringLiteral("^\\d+\\. \\[( |x)\\] (.*)$"));
    static const QRegularExpression plain(QStringLiteral("^\\d+\\. (.*)$"));
    QVariantList criteria;
    bool inside = false;
    for (const QString& line : lines) {
        if (line == QStringLiteral("Acceptance criteria:")) {
            inside = true;
            continue;
        }
        if (!inside)
            continue;
        if (line.isEmpty())
            break;
        if (const auto match = checked.match(line); match.hasMatch())
            criteria.append(QVariantMap{{QStringLiteral("text"), match.captured(2)},
                                        {QStringLiteral("done"), match.captured(1) == "x"},
                                        {QStringLiteral("checked"), true}});
        else if (const auto loose = plain.match(line); loose.hasMatch())
            criteria.append(QVariantMap{{QStringLiteral("text"), loose.captured(1)},
                                        {QStringLiteral("done"), false},
                                        {QStringLiteral("checked"), false}});
    }
    return criteria;
}

// The paragraph after the heading lines, up to the next labelled part.
QString descriptionOf(const QStringList& lines) {
    qsizetype start = lines.indexOf(QString{});
    if (start < 0)
        return {};
    QStringList text;
    for (qsizetype index = start + 1; index < lines.size(); ++index) {
        const QString& line = lines.at(index);
        if (line.isEmpty() || line.endsWith(QLatin1Char(':')) || line.startsWith("Start it with"))
            break;
        text.append(line);
    }
    return text.join(QLatin1Char('\n'));
}

QVariantMap assignment(const QString& head, const QStringList& lines) {
    static const QRegularExpression pattern(
        QStringLiteral("^task (T-\\d+) \"(.*)\" (is assigned to you|was handed over to you) by "
                       "(.*?)(?:\\. Reason: (.*)|\\.)$"));
    const auto match = pattern.match(head);
    if (!match.hasMatch())
        return {};
    const bool handover = match.captured(3).startsWith(QStringLiteral("was"));
    QStringList facts;
    if (const QString lane = valueAfter(lines, QStringLiteral("Lane:"));
        !lane.isEmpty() && lane != QStringLiteral("(none)"))
        facts.append(QStringLiteral("Lane ") + lane);
    if (const QString branch = valueAfter(lines, QStringLiteral("Branch:")); !branch.isEmpty())
        facts.append(branch);
    facts.append((handover ? QStringLiteral("Handed over by ") : QStringLiteral("Assigned by ")) +
                 match.captured(4));
    QString text = descriptionOf(lines);
    if (handover && !match.captured(5).isEmpty())
        text = QStringLiteral("Reason: ") + match.captured(5) +
               (text.isEmpty() ? QString{} : QStringLiteral("\n\n") + text);
    return {{QStringLiteral("kind"), QStringLiteral("assignment")},
            {QStringLiteral("taskId"), match.captured(1)},
            {QStringLiteral("title"), match.captured(2)},
            {QStringLiteral("facts"), facts},
            {QStringLiteral("text"), text},
            {QStringLiteral("criteria"), criteriaOf(lines)}};
}

QVariantMap review(const QString& head, const QStringList& lines) {
    static const QRegularExpression request(
        QStringLiteral("^review task (T-\\d+) \"(.*)\" \\(round (\\d+) of (\\d+)\\)\\.$"));
    static const QRegularExpression verdict(QStringLiteral(
        "^the review of (T-\\d+) \"(.*)\" \\(round (\\d+) of (\\d+)\\) found issues\\.$"));
    if (const auto match = request.match(head); match.hasMatch()) {
        QStringList facts{
            QStringLiteral("Round %1 of %2").arg(match.captured(3), match.captured(4))};
        if (const QString branch = valueAfter(lines, QStringLiteral("Branch:")); !branch.isEmpty())
            facts.append(branch);
        QStringList files;
        if (const QString changed = valueAfter(lines, QStringLiteral("Changed files:"));
            !changed.isEmpty() && changed != QStringLiteral("none"))
            files = changed.split(QStringLiteral(", "), Qt::SkipEmptyParts);
        return {{QStringLiteral("kind"), QStringLiteral("review")},
                {QStringLiteral("taskId"), match.captured(1)},
                {QStringLiteral("title"), match.captured(2)},
                {QStringLiteral("facts"), facts},
                {QStringLiteral("text"), descriptionOf(lines)},
                {QStringLiteral("criteria"), criteriaOf(lines)},
                {QStringLiteral("files"), files}};
    }
    const auto match = verdict.match(head);
    if (!match.hasMatch())
        return {};
    // The findings, as the reviewer wrote them, without the daemon's closing instruction.
    QStringList findings;
    for (qsizetype index = 1; index < lines.size(); ++index) {
        const QString& line = lines.at(index);
        if (line.startsWith(QStringLiteral("Fix them, commit")))
            break;
        findings.append(line);
    }
    return {{QStringLiteral("kind"), QStringLiteral("verdict")},
            {QStringLiteral("taskId"), match.captured(1)},
            {QStringLiteral("title"), match.captured(2)},
            {QStringLiteral("facts"), QStringList{QStringLiteral("Round %1 of %2 found issues")
                                                      .arg(match.captured(3), match.captured(4)),
                                                  QStringLiteral("Back to In progress")}},
            {QStringLiteral("text"), findings.join(QLatin1Char('\n')).trimmed()}};
}

QVariantMap digest(const QString& head, const QStringList& lines) {
    if (head != QStringLiteral("what changed since your last digest."))
        return {};
    static const QRegularExpression taskHeading(QStringLiteral("^(T-\\d+) \"(.*)\" \\((\\w+)\\)$"));
    QVariantList groups;
    QVariantMap group;
    QVariantList groupLines;
    int changes = 0;
    const auto flush = [&] {
        if (!groupLines.isEmpty()) {
            group.insert(QStringLiteral("lines"), groupLines);
            groups.append(group);
        }
        group.clear();
        groupLines.clear();
    };
    for (qsizetype index = 1; index < lines.size(); ++index) {
        const QString& line = lines.at(index);
        if (line.isEmpty()) {
            flush();
            continue;
        }
        if (line.startsWith(QStringLiteral("- "))) {
            if (group.isEmpty())
                continue;
            const bool needsYou = line.startsWith(QStringLiteral("- [needs you] "));
            groupLines.append(QVariantMap{{QStringLiteral("text"), line.mid(needsYou ? 14 : 2)},
                                          {QStringLiteral("needsYou"), needsYou}});
            ++changes;
            continue;
        }
        if (!group.isEmpty())
            continue;
        if (const auto match = taskHeading.match(line); match.hasMatch())
            group = {{QStringLiteral("taskId"), match.captured(1)},
                     {QStringLiteral("title"), match.captured(2)},
                     {QStringLiteral("state"), stateLabel(match.captured(3))}};
        else if (!line.startsWith(QStringLiteral("Act on the")))
            group = {{QStringLiteral("taskId"), QString{}},
                     {QStringLiteral("title"), line},
                     {QStringLiteral("state"), QString{}}};
    }
    flush();
    if (groups.isEmpty())
        return {};
    return {{QStringLiteral("kind"), QStringLiteral("digest")},
            {QStringLiteral("taskId"), QString{}},
            {QStringLiteral("title"),
             changes == 1 ? QStringLiteral("One change since your last digest")
                          : QStringLiteral("%1 changes since your last digest").arg(changes)},
            {QStringLiteral("groups"), groups},
            {QStringLiteral("changes"), changes}};
}

QVariantMap decision(const QString& head, const QStringList& lines) {
    static const QRegularExpression pattern(QStringLiteral("^the user answered (D-\\d+)\\.$"));
    static const QRegularExpression goesOn(QStringLiteral("^Task (T-\\d+) can go on\\.$"));
    const auto match = pattern.match(head);
    if (!match.hasMatch())
        return {};
    QString taskId;
    for (const QString& line : lines)
        if (const auto task = goesOn.match(line); task.hasMatch())
            taskId = task.captured(1);
    return {{QStringLiteral("kind"), QStringLiteral("decision")},
            {QStringLiteral("taskId"), taskId},
            {QStringLiteral("decisionId"), match.captured(1)},
            {QStringLiteral("title"), match.captured(1) + QStringLiteral(" answered")},
            {QStringLiteral("question"), valueAfter(lines, QStringLiteral("Question:"))},
            {QStringLiteral("answer"), valueAfter(lines, QStringLiteral("Answer:"))},
            {QStringLiteral("text"),
             taskId.isEmpty() ? QString{} : QStringLiteral("%1 can go on.").arg(taskId)}};
}

} // namespace

QVariantMap fromMessage(const QJsonObject& agentMessage) {
    const QString kind = agentMessage.value(QStringLiteral("kind")).toString();
    QStringList lines =
        agentMessage.value(QStringLiteral("body")).toString().split(QLatin1Char('\n'));
    for (QString& line : lines)
        line = line.trimmed();
    static const QRegularExpression heading(QStringLiteral("^Team (.+?): (.*)$"));
    const auto match = lines.isEmpty() ? QRegularExpressionMatch{} : heading.match(lines.first());
    if (!match.hasMatch())
        return {};
    const QString head = match.captured(2);
    QVariantMap block;
    if (kind == QStringLiteral("assignment"))
        block = assignment(head, lines);
    else if (kind == QStringLiteral("review"))
        block = review(head, lines);
    else if (kind == QStringLiteral("digest"))
        block = digest(head, lines);
    else if (kind == QStringLiteral("decision"))
        block = decision(head, lines);
    if (block.isEmpty())
        return {};
    block.insert(QStringLiteral("team"), match.captured(1));
    block.insert(QStringLiteral("accessibleName"), accessibleName(block));
    return block;
}

QVariantMap fromAgentMessages(const QJsonArray& agentMessages) {
    if (agentMessages.size() != 1)
        return {};
    return fromMessage(agentMessages.first().toObject());
}

QString accessibleName(const QVariantMap& block) {
    static const QHash<QString, QString> kinds{
        {QStringLiteral("assignment"), QStringLiteral("Assignment")},
        {QStringLiteral("digest"), QStringLiteral("Digest")},
        {QStringLiteral("review"), QStringLiteral("Review request")},
        {QStringLiteral("verdict"), QStringLiteral("Review")},
        {QStringLiteral("decision"), QStringLiteral("Decision")}};
    const QString task = block.value(QStringLiteral("taskId")).toString();
    return QStringLiteral("%1: %2").arg(kinds.value(block.value(QStringLiteral("kind")).toString()),
                                        (task.isEmpty() ? QString{} : task + QLatin1Char(' ')) +
                                            block.value(QStringLiteral("title")).toString());
}

} // namespace ketplus::team_blocks
