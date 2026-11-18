#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <algorithm>

namespace team_reply_test {

// Fixture-only projection: never serialize a session, approval or provider response whole.
// Keep the latest synthetic worktree history while excluding prompts, tool inputs and auth.
inline QJsonObject failureMetadata(const QString& phase, const QJsonObject& team,
                                   const QJsonArray& owners, const QString& fixtureRoot) {
    const auto text = [&](QString value, int limit = 128) {
        value.replace(fixtureRoot, "<fixture>");
        return value.left(limit);
    };
    QJsonArray tasks;
    const auto sourceTasks = team.value("tasks").toArray();
    for (const auto& value : sourceTasks) {
        if (tasks.size() == 8)
            break;
        const auto task = value.toObject();
        QJsonArray history;
        const auto entries = task.value("history").toArray();
        for (qsizetype i = std::max<qsizetype>(0, entries.size() - 6); i < entries.size(); ++i) {
            const auto entry = entries.at(i).toObject();
            QJsonObject row{{"kind", text(entry.value("kind").toString(), 40)},
                            {"actor", text(entry.value("actor").toString())},
                            {"at", text(entry.value("at").toString(), 48)}};
            if (entry.value("kind") == "worktree")
                row.insert("worktree_error", text(entry.value("text").toString(), 384));
            history.append(row);
        }
        tasks.append(QJsonObject{{"id", text(task.value("id").toString())},
                                 {"owner", text(task.value("owner").toString())},
                                 {"state", text(task.value("state").toString(), 40)},
                                 {"worktree", text(task.value("worktree").toString(), 256)},
                                 {"history_total", entries.size()},
                                 {"history", history}});
    }
    QJsonArray sessions;
    for (const auto& value : owners) {
        if (sessions.size() == 4)
            break;
        const auto owner = value.toObject();
        QJsonArray approvals;
        for (const auto& pending : owner.value("pending_approvals").toArray()) {
            if (approvals.size() == 8)
                break;
            const auto approval = pending.toObject();
            approvals.append(
                QJsonObject{{"kind", text(approval.value("kind").toString(), 40)},
                            {"method", text(approval.value("method").toString())},
                            {"request_type", text(approval.value("request_type").toString())}});
        }
        QJsonObject row{{"id", text(owner.value("id").toString())},
                        {"state", text(owner.value("state").toString(), 40)},
                        {"adapter", text(owner.value("adapter").toString(), 40)},
                        {"worktree", text(owner.value("worktree").toString(), 256)},
                        {"pending_approvals", approvals}};
        for (const auto* key :
             {"generation", "context_revision", "turn_sequence", "completed_turns"})
            row.insert(key, owner.value(key).toInteger());
        row.insert("pending_approvals_has_more",
                   owner.value("pending_approvals_has_more").toBool());
        row.insert("pending_approvals_read_ok", owner.value("ok").toBool());
        row.insert("pending_approvals_error_code", text(owner.value("error_code").toString(), 64));
        sessions.append(row);
    }
    return {{"phase", phase.left(64)},
            {"paused", team.value("paused").toBool()},
            {"task_count", sourceTasks.size()},
            {"tasks", tasks},
            {"owners", sessions}};
}

} // namespace team_reply_test
