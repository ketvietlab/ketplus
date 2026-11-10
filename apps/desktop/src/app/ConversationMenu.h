#pragma once

#include <QString>

class QAction;
class QMenu;

namespace ketplus {

// The sidebar's conversation menu, as the mock draws it (Sidebar.qml rowMenu).
struct ConversationMenuActions {
    QAction* rename = nullptr;
    QAction* fork = nullptr;
    QAction* changeWorktree = nullptr;
    QAction* copyReference = nullptr;
    QAction* archive = nullptr;
    QAction* remove = nullptr;
};

// `reference` is "@agent/N", or empty when the conversation has no number yet.
ConversationMenuActions addConversationMenuActions(QMenu& menu, bool forkable, bool running,
                                                   const QString& reference);

} // namespace ketplus
