#include "teams/TeamAgentTasks.h"

#include <algorithm>

namespace ketplus::daemon::teams {
namespace {

QString readHint(const Task& task, const qsizetype index) {
    return QStringLiteral("team_tasks task_id=%1 entry=%2 reads it whole").arg(task.id).arg(index);
}

// Strings in an entry's data, such as a verdict's findings, cut like its text.
QJsonValue cutValue(const QJsonValue& value, const QString& hint) {
    if (value.isString())
        return cutText(value.toString(), agentEntryText, hint);
    if (value.isObject()) {
        QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            it.value() = cutValue(it.value(), hint);
        return object;
    }
    if (value.isArray()) {
        QJsonArray array = value.toArray();
        for (qsizetype index = 0; index < array.size(); ++index)
            array.replace(index, cutValue(array.at(index), hint));
        return array;
    }
    return value;
}

QJsonObject entryJson(const HistoryEntry& entry, const qsizetype index) {
    QJsonObject json = toJson(entry);
    json.insert(QStringLiteral("entry"), index);
    return json;
}

} // namespace

QJsonObject agentTaskRow(const Task& task, const AgentMember& owner) {
    QJsonObject row{{QStringLiteral("id"), task.id},
                    {QStringLiteral("title"), task.title},
                    {QStringLiteral("state"), task.state},
                    {QStringLiteral("round"), task.round},
                    {QStringLiteral("lane"), task.lane},
                    {QStringLiteral("depends_on"), QJsonArray::fromStringList(task.dependsOn)},
                    {QStringLiteral("pull_request"), task.pullRequest}};
    row.insert(QStringLiteral("owner"),
               task.owner.isEmpty()
                   ? QJsonValue(QJsonValue::Null)
                   : QJsonValue(QJsonObject{{QStringLiteral("number"), owner.number},
                                            {QStringLiteral("name"), owner.name}}));
    if (task.state == state::blocked)
        row.insert(QStringLiteral("question"), task.question);
    return row;
}

QJsonArray agentTaskRows(const Team& team, const AgentTaskFilter& filter,
                         const MemberLookup& members) {
    QJsonArray rows;
    for (const Task& task : team.tasks) {
        if (filter.open && task.state == state::done)
            continue;
        if (!filter.mine.isEmpty() && task.owner != filter.mine && task.reviewer != filter.mine)
            continue;
        rows.append(agentTaskRow(task, task.owner.isEmpty() ? AgentMember{} : members(task.owner)));
    }
    return rows;
}

QJsonObject agentTaskPage(const Task& task, const qsizetype start, const int limit) {
    QJsonObject json = summaryJson(task);
    const qsizetype total = task.history.size();
    const qsizetype count = std::clamp(limit, 1, agentHistoryMaximum);
    const qsizetype first =
        start < 0 ? std::max<qsizetype>(0, total - count) : std::min(start, total);
    const qsizetype end = std::min(total, first + count);
    QJsonArray history;
    for (qsizetype index = first; index < end; ++index) {
        QJsonObject entry = entryJson(task.history.at(index), index);
        if (entry.contains(QStringLiteral("data")))
            entry.insert(QStringLiteral("data"),
                         cutValue(entry.value(QStringLiteral("data")), readHint(task, index)));
        history.append(entry);
    }
    json.insert(QStringLiteral("history"), history);
    json.insert(QStringLiteral("history_total"), total);
    json.insert(QStringLiteral("history_start"), first);
    return json;
}

QJsonObject agentTaskEntry(const Task& task, const qsizetype index,
                           const std::optional<QString>& wholeText) {
    QJsonObject entry = entryJson(task.history.at(index), index);
    if (wholeText)
        entry.insert(QStringLiteral("text"), *wholeText);
    return {{QStringLiteral("id"), task.id},
            {QStringLiteral("entry"), entry},
            {QStringLiteral("history_total"), task.history.size()}};
}

} // namespace ketplus::daemon::teams
