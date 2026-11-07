#include "agent/AgentWorkspaceView.h"

namespace ketplus {
void AgentWorkspaceView::requestHeldMessageState(const QString& messageId) {
    if (messageId.isEmpty())
        return;
    auto state = heldMessageStates_.value(messageId).toMap();
    if (state.value("pending").toBool())
        return;
    state.insert("pending", true);
    heldMessageStates_.insert(messageId, state);
    emit heldMessageStatesChanged();
    emit heldMessageActionRequested(messageId, false);
}
void AgentWorkspaceView::releaseHeldMessage(const QString& messageId) {
    auto state = heldMessageStates_.value(messageId).toMap();
    if (messageId.isEmpty() || state.value("pending").toBool() ||
        !state.value("can_release").toBool())
        return;
    state.insert("pending", true);
    state.remove("error");
    heldMessageStates_.insert(messageId, state);
    emit heldMessageStatesChanged();
    emit heldMessageActionRequested(messageId, true);
}
void AgentWorkspaceView::acceptHeldMessageState(const QString& messageId,
                                                const QVariantMap& value) {
    auto state = value;
    state.insert("pending", false);
    heldMessageStates_.insert(messageId, state);
    emit heldMessageStatesChanged();
}
} // namespace ketplus
