#include "MachineRows.h"

#include <QFileInfo>
#include <QJsonArray>

namespace ketplus::remote {

namespace {

// The first non-empty string among the spellings the routes use for one field.
QString text(const QJsonObject& row, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const QJsonValue value = row.value(QLatin1String(key));
        if (value.isString() && !value.toString().isEmpty())
            return value.toString();
        if (value.isDouble())
            return QString::number(value.toInteger());
    }
    return {};
}

qint64 integer(const QJsonObject& row, std::initializer_list<const char*> keys) {
    for (const char* key : keys)
        if (row.value(QLatin1String(key)).isDouble())
            return row.value(QLatin1String(key)).toInteger();
    return 0;
}

MachineWorktree worktreeFrom(const QJsonObject& row, const QString& repositoryId) {
    MachineWorktree worktree;
    worktree.id = text(row, {"id", "worktreeId", "worktree_id", "path", "worktree_path"});
    worktree.path = text(row, {"path", "worktree_path"});
    worktree.label = text(row, {"label", "name", "worktree_label"});
    if (worktree.label.isEmpty())
        worktree.label =
            worktree.path.isEmpty() ? worktree.id : QFileInfo(worktree.path).fileName();
    worktree.branch = text(row, {"branch"});
    worktree.repositoryId = text(row, {"repository_id", "repositoryId"});
    if (worktree.repositoryId.isEmpty())
        worktree.repositoryId = repositoryId;
    if (row.contains(QLatin1String("available")))
        worktree.available = row.value(QLatin1String("available")).toBool();
    else if (row.contains(QLatin1String("exists")))
        worktree.available = row.value(QLatin1String("exists")).toBool(true);
    return worktree;
}

} // namespace

QVector<MachineRepository> repositoriesFromListing(const QJsonObject& result) {
    QVector<MachineRepository> repositories;
    for (const QJsonValue& value : result.value(QLatin1String("repositories")).toArray()) {
        const QJsonObject row = value.toObject();
        MachineRepository repository;
        repository.id = text(row, {"id", "repositoryId"});
        if (repository.id.isEmpty())
            continue;
        repository.path = text(row, {"path"});
        repository.label = text(row, {"label"});
        if (repository.label.isEmpty())
            repository.label = repository.id;
        repository.available = row.value(QLatin1String("available")).toBool(true);
        if (row.contains(QLatin1String("worktrees"))) {
            for (const QJsonValue& item : row.value(QLatin1String("worktrees")).toArray()) {
                MachineWorktree worktree = worktreeFrom(item.toObject(), repository.id);
                if (!worktree.id.isEmpty())
                    repository.worktrees.append(worktree);
            }
        } else {
            repository.worktrees.append({repository.id,
                                         repository.label,
                                         repository.path,
                                         {},
                                         repository.id,
                                         repository.available,
                                         false});
        }
        repositories.append(repository);
    }
    return repositories;
}

QVector<MachineWorktree> pendingWorktreesFromListing(const QJsonObject& result) {
    QVector<MachineWorktree> pending;
    for (const QJsonValue& value : result.value(QLatin1String("pending")).toArray()) {
        const QJsonObject row = value.toObject();
        if (!row.value(QLatin1String("kind"))
                 .toString(QStringLiteral("worktree"))
                 .startsWith(QLatin1String("worktree")))
            continue;
        MachineWorktree worktree = worktreeFrom(row, {});
        if (worktree.id.isEmpty())
            continue;
        worktree.pending = true;
        worktree.available = false;
        pending.append(worktree);
    }
    return pending;
}

QVector<MachineWorktree> worktreesFromPage(const QJsonObject& result) {
    QVector<MachineWorktree> worktrees;
    for (const QJsonValue& value : result.value(QLatin1String("worktrees")).toArray()) {
        MachineWorktree worktree = worktreeFrom(value.toObject(), {});
        if (!worktree.id.isEmpty())
            worktrees.append(worktree);
    }
    return worktrees;
}

QVector<MachineConversation> conversationsFromListing(const QJsonObject& result) {
    QVector<MachineConversation> conversations;
    for (const QJsonValue& value : result.value(QLatin1String("sessions")).toArray()) {
        const QJsonObject row = value.toObject();
        MachineConversation conversation;
        conversation.sessionId = text(row, {"session_id", "id"});
        if (conversation.sessionId.isEmpty() || row.contains(QLatin1String("archived_at")))
            continue;
        conversation.generation = integer(row, {"session_generation", "generation"});
        conversation.title = text(row, {"title"});
        conversation.number = text(row, {"number"});
        conversation.worktreeId = text(row, {"worktree_path", "worktreeId"});
        conversation.worktreeLabel = text(row, {"worktreeLabel"});
        if (conversation.worktreeLabel.isEmpty())
            conversation.worktreeLabel = QFileInfo(conversation.worktreeId).fileName();
        conversation.repositoryId = text(row, {"repository_id", "repositoryId"});
        conversation.state = text(row, {"state"});
        conversation.awaitingReply = row.value(QLatin1String("awaiting_reply")).toBool();
        conversations.append(conversation);
    }
    return conversations;
}

QJsonObject normalizedEvent(const QJsonObject& event) {
    QJsonObject normalized = event;
    const auto adopt = [&](const char* local, const char* relay) {
        const QLatin1String to(local), from(relay);
        if (!normalized.contains(to) && normalized.contains(from))
            normalized.insert(to, normalized.value(from));
        normalized.remove(from);
    };
    adopt("session_id", "sessionId");
    adopt("session_generation", "sessionGeneration");
    adopt("event_type", "eventType");
    adopt("turn_sequence", "turnSequence");
    return normalized;
}

QString queuedCommandId(const QJsonObject& result) {
    if (result.value(QLatin1String("status")).toString() != QLatin1String("queued"))
        return {};
    return result.value(QLatin1String("command_id")).toString();
}

MachineConversation createdSession(const QJsonObject& result) {
    MachineConversation conversation;
    conversation.sessionId = text(result, {"session_id", "sessionId"});
    conversation.generation =
        integer(result, {"session_generation", "sessionGeneration", "generation"});
    return conversation;
}

} // namespace ketplus::remote
