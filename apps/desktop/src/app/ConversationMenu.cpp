#include "app/ConversationMenu.h"

#include "KetIcons.h"
#include "KetStyle.h"

#include <QAction>
#include <QCoreApplication>
#include <QMenu>

namespace ketplus {

ConversationMenuActions addConversationMenuActions(QMenu& menu, const bool forkable,
                                                   const bool running, const QString& reference) {
    const auto tr = [](const char* text) {
        return QCoreApplication::translate("ConversationMenu", text);
    };
    const auto entry = [&menu](const QString& text, QStringView icon, const char* name) {
        auto* action = menu.addAction(ket::Icons::icon(icon), text);
        action->setObjectName(QLatin1String(name));
        return action;
    };
    ConversationMenuActions actions;
    actions.rename = entry(tr("Rename…"), u"edit", "renameConversationAction");
    actions.fork = entry(tr("Fork conversation"), u"branch", "forkConversationAction");
    actions.fork->setEnabled(forkable);
    actions.changeWorktree =
        entry(tr("Change worktree…"), u"branch", "changeConversationWorktreeAction");
    // The reference other conversations use to message this one; ket::Style draws the text
    // after the tab as the item's shortcut.
    actions.copyReference = entry(tr("Copy reference") + QLatin1Char('\t') + reference, u"copy",
                                  "copyConversationReferenceAction");
    actions.copyReference->setVisible(!reference.isEmpty());
    menu.addSeparator();
    actions.archive = entry(tr("Archive"), u"archive", "archiveConversationAction");
    actions.remove = entry(tr("Delete…"), u"trash", "deleteConversationAction");
    actions.remove->setProperty(ket::Style::kDangerProperty, true);
    actions.remove->setEnabled(!running);
    if (running)
        actions.remove->setProperty(ket::Style::kDescriptionProperty,
                                    tr("Stop the conversation before deleting it."));
    return actions;
}

} // namespace ketplus
