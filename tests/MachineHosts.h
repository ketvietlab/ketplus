#pragma once

#include "FakeMachineBackend.h"
#include "machine/MachineWorkspaceView.h"

#include <QJsonArray>
#include <QListWidget>

// The two hosts the machine composition tests talk to: a Linux host over SSH (the local
// protocol's spelling) and one through the Relay (salted ids, the Relay's spelling).
namespace ketplus::test {

using Route = remote::MachineBackend::Route;
using State = remote::MachineBackend::ConnectionState;

inline QJsonObject journalEvent(const QString& session, qint64 cursor, const QString& type,
                                const QJsonObject& payload) {
    return {{"session_id", session},
            {"session_generation", 1},
            {"cursor", cursor},
            {"event_type", type},
            {"payload", payload}};
}

// A Linux host over SSH: the daemon's local protocol, a repository with a linked worktree.
inline FakeMachineBackend::Answer sshHost() {
    return [](const FakeMachineBackend::Call& call, QJsonObject* result, QString*) {
        if (call.operation == "listRepositories") {
            *result = {{"repositories",
                        QJsonArray{QJsonObject{
                            {"id", "/home/dev/app"},
                            {"label", "app"},
                            {"path", "/home/dev/app"},
                            {"available", true},
                            {"worktrees", QJsonArray{QJsonObject{{"id", "/home/dev/app"},
                                                                 {"label", "app"},
                                                                 {"path", "/home/dev/app"},
                                                                 {"available", true}},
                                                     QJsonObject{{"id", "/home/dev/app-fix"},
                                                                 {"label", "app-fix"},
                                                                 {"path", "/home/dev/app-fix"},
                                                                 {"available", true}}}}}}}};
        } else if (call.operation == "listConversations") {
            *result = {{"sessions", QJsonArray{QJsonObject{{"session_id", "linux-1"},
                                                           {"session_generation", 1},
                                                           {"worktree_path", "/home/dev/app-fix"},
                                                           {"title", "Fix the build"},
                                                           {"state", "ready"}}}}};
        } else if (call.operation == "transcriptWindow") {
            *result = {
                {"events",
                 QJsonArray{journalEvent(call.sessionId, 1, "user.message", {{"text", "Build it"}}),
                            journalEvent(call.sessionId, 2, "assistant.delta",
                                         {{"item_id", "a"}, {"delta", "Built on Linux."}})}}};
        } else if (call.operation == "pendingApprovals") {
            *result = {{"pending_approvals", QJsonArray{}}};
        } else if (call.operation == "createSession") {
            *result = {{"session_id", "linux-2"}, {"session_generation", 1}};
        }
    };
}

// A host through the Relay: salted ids, no host paths, one worktree still verifying.
inline FakeMachineBackend::Answer relayHost(bool* verified) {
    return [verified](const FakeMachineBackend::Call& call, QJsonObject* result, QString*) {
        if (call.operation == "listRepositories") {
            QJsonArray worktrees{QJsonObject{{"worktreeId", "wt-main"}, {"label", "main"}}};
            QJsonArray pending;
            if (*verified)
                worktrees.append(QJsonObject{{"worktreeId", "wt-new"}, {"label", "feature"}});
            else
                pending.append(QJsonObject{{"kind", "worktree"},
                                           {"id", "wt-new"},
                                           {"machineId", "relay-machine"},
                                           {"reason", "verification_pending"}});
            *result = {{"repositories", QJsonArray{QJsonObject{{"id", "repo-salted"},
                                                               {"label", "site"},
                                                               {"available", true},
                                                               {"worktrees", worktrees}}}},
                       {"pending", pending}};
        } else if (call.operation == "listConversations") {
            *result = {{"sessions", QJsonArray{QJsonObject{{"id", "relay-1"},
                                                           {"generation", 4},
                                                           {"repositoryId", "repo-salted"},
                                                           {"worktreeId", "wt-main"},
                                                           {"title", "Deploy"}}}}};
        } else if (call.operation == "pendingApprovals") {
            *result = {{"pending_approvals", QJsonArray{}}};
        }
    };
}

inline QStringList transcriptRows(const MachineWorkspaceView& view) {
    QStringList rows;
    auto* list = view.findChild<QListWidget*>("machineTranscript");
    for (int i = 0; i < list->count(); ++i)
        rows.append(list->item(i)->text());
    return rows;
}

} // namespace ketplus::test
