#include "machine/MachineSidebar.h"

#include "machine/MachineCatalog.h"

#include <QHash>
#include <QObject>

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
        QString repository = catalog.repositoryOf(conversation);
        auto index = indexById.constFind(repository);
        if (index == indexById.cend()) {
            // A conversation outside every repository the machine named (a directory that is
            // not a registered workspace) stays listed, under its own worktree.
            repository = conversation.worktreeId.isEmpty() ? QStringLiteral("other:")
                                                           : conversation.worktreeId;
            index = indexById.constFind(repository);
            if (index == indexById.cend()) {
                index = indexById.insert(repository, repositories.size());
                repositories.append({repository,
                                     conversation.worktreeId.isEmpty()
                                         ? QObject::tr("Other conversations")
                                         : conversation.worktreeLabel,
                                     false,
                                     {},
                                     false,
                                     {}});
            }
        }
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
