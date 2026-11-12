#pragma once

#include "workspace/SidebarRepositoryList.h"

#include <QVector>

namespace ketplus {

class MachineCatalog;

// ADR 0119 "Sidebar": the selected machine's repositories, each with its conversations, in the
// shape the sidebar shows for This Mac. A repository is keyed by its id (the host path locally
// and over SSH, the salted id on the Relay) and a conversation row's `path` is its worktree id;
// `sessionId` and `id` name the conversation on that machine.
[[nodiscard]] QVector<SidebarRepository> machineSidebarRepositories(const MachineCatalog& catalog,
                                                                    const QString& openSessionId);

} // namespace ketplus
