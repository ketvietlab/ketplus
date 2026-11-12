#include "machine/MachineSidebar.h"

#include "machine/MachineCatalog.h"

#include <QHash>

namespace ketplus {

QVector<SidebarRepository> machineSidebarRepositories(const MachineCatalog& catalog,
                                                      const QString& openSessionId) {
    QVector<SidebarRepository> repositories;
    QHash<QString, qsizetype> indexById;
    for (const auto& repository : catalog.repositories()) {
        indexById.insert(repository.id, repositories.size());
        repositories.append({repository.id, repository.label, false, {}, false, {}});
    }
    for (const auto& conversation : catalog.conversations()) {
        const QString repository = catalog.repositoryOf(conversation);
        const auto index = indexById.constFind(repository);
        if (index == indexById.cend())
            continue; // a conversation the machine lists under no repository it named
        const bool selected = conversation.sessionId == openSessionId;
        repositories[*index].conversations.append(
            QVariantMap{{QStringLiteral("id"), conversation.sessionId},
                        {QStringLiteral("sessionId"), conversation.sessionId},
                        {QStringLiteral("path"), conversation.worktreeId},
                        {QStringLiteral("title"), conversation.title},
                        {QStringLiteral("number"), conversation.number},
                        {QStringLiteral("running"), conversation.running()},
                        {QStringLiteral("awaitingReply"), conversation.awaitingReply},
                        {QStringLiteral("started"), true},
                        {QStringLiteral("listed"), true},
                        {QStringLiteral("active"), selected},
                        {QStringLiteral("selected"), selected}});
        if (selected)
            repositories[*index].active = true;
    }
    return repositories;
}

} // namespace ketplus
