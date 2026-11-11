#include "KetButton.h"
#include "KetIcons.h"
#include "agent/AgentWorkspaceView.h"
#include "app/MainWindow.h"

#include <QHBoxLayout>
#include <QMenu>
#include <QTabBar>
#include <QTabWidget>

namespace ketplus {

// The mock's DetailsPane actions beside its tabs: Open content (the openers the header hides
// while the pane is open), Open tabs and Hide content pane.
void MainWindow::installAgentDetailActions() {
    auto* actions = new QWidget(agentDetailTabs_);
    actions->setObjectName(QStringLiteral("agentDetailActions"));
    auto* layout = new QHBoxLayout(actions);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(0);
    const auto button = [actions, layout](const char16_t* icon, const QString& text,
                                          const QString& name) {
        auto* action =
            new ket::IconButton(ket::Icons::icon(ket::Icons::codeForLucide(icon)), text, actions);
        action->setObjectName(name);
        layout->addWidget(action);
        return action;
    };
    auto* open = button(u"plus", tr("Open content"), QStringLiteral("agentDetailOpenContent"));
    auto* tabs = button(u"chevron-down", tr("Open tabs"), QStringLiteral("agentDetailOpenTabs"));
    auto* hide = button(u"panel-right-close", tr("Hide content pane"),
                        QStringLiteral("agentDetailHidePane"));
    connect(open, &QAbstractButton::clicked, this, [this, open] {
        QMenu menu(open);
        menu.setObjectName(QStringLiteral("agentDetailOpenContentMenu"));
        auto* terminal = menu.addAction(tr("New terminal"));
        auto* changes = menu.addAction(tr("Changes"));
        auto* report = menu.addAction(tr("Work report"));
        const auto* chosen = menu.exec(open->mapToGlobal(QPoint(0, open->height())));
        if (chosen == terminal)
            agentWorkspace_->requestTerminal();
        else if (chosen == changes)
            openAgentChangesTab();
        else if (chosen == report)
            openAgentWorkReportTab();
    });
    connect(tabs, &QAbstractButton::clicked, this, [this, tabs] {
        QMenu menu(tabs);
        menu.setObjectName(QStringLiteral("agentDetailOpenTabsMenu"));
        for (int index = 0; index < agentDetailTabs_->count(); ++index) {
            auto* action = menu.addAction(agentDetailTabs_->tabText(index));
            action->setCheckable(true);
            action->setChecked(index == agentDetailTabs_->currentIndex());
            connect(action, &QAction::triggered, agentDetailTabs_,
                    [this, index] { agentDetailTabs_->setCurrentIndex(index); });
        }
        menu.exec(tabs->mapToGlobal(QPoint(0, tabs->height())));
    });
    connect(hide, &QAbstractButton::clicked, this, &MainWindow::hideAgentDetailPane);
    agentDetailTabs_->setCornerWidget(actions, Qt::TopRightCorner);
}

void MainWindow::hideAgentDetailPane() {
    agentDetailPaneHidden_ = true;
    updateAgentDetailTabs();
    if (agentWorkspace_ != nullptr)
        agentWorkspace_->focusPrompt();
}

} // namespace ketplus
