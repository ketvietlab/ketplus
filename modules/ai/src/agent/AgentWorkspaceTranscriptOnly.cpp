// ketplus#598: the Agent Workspace showing only the transcript of a conversation that lives on
// another machine.
#include "agent/AgentWorkspaceView.h"

namespace ketplus {

void AgentWorkspaceView::setTranscriptOnly(const QString& localActionReason) {
    if (localActionReason_ == localActionReason)
        return;
    localActionReason_ = localActionReason;
    emit transcriptOnlyChanged();
    emit pathStatesChanged();
}

bool AgentWorkspaceView::localActionRefused() {
    if (localActionReason_.isEmpty())
        return false;
    emit composerErrorOccurred(localActionReason_);
    return true;
}

} // namespace ketplus
