#include "agent/AgentWorkspaceView.h"

namespace ketplus {

// ADR 0067: the reviewer of the active conversation, read again whenever another
// conversation becomes active or the active one starts its first turn.
void AgentWorkspaceView::setReviewerScope(const QString& scope) {
    if (reviewerScope_ == scope)
        return;
    reviewerScope_ = scope;
    reviewerState_ = scope.isEmpty() ? QVariantMap{} : QVariantMap{{"pending", true}};
    emit reviewerStateChanged();
    if (!scope.isEmpty())
        emit reviewerRequested();
}

QVariantList AgentWorkspaceView::reviewerCandidates() const {
    if (!conversationDirectory_)
        return {};
    QString active;
    for (const QVariant& value : conversations_)
        if (const auto conversation = value.toMap(); conversation.value("active").toBool())
            active = conversation.value("number").toString();
    QVariantList candidates;
    for (const QVariant& value : conversationDirectory_()) {
        auto conversation = value.toMap();
        const QString number = conversation.value("number").toString();
        const QString provider = conversation.value("provider").toString();
        // Only Claude enforces the read-only review guard (ADR 0067).
        if (number.isEmpty() || number == active || provider != QStringLiteral("claude"))
            continue;
        conversation.insert("worktree", conversation.value("path").toString().section(
                                            QLatin1Char('/'), -1, -1, QString::SectionSkipEmpty));
        candidates.append(conversation);
    }
    return candidates;
}

void AgentWorkspaceView::chooseReviewer(const QString& number, const QString& newProvider) {
    if (reviewerScope_.isEmpty() || reviewerState_.value("pending").toBool())
        return;
    reviewerState_.insert("pending", true);
    reviewerState_.remove("error");
    emit reviewerStateChanged();
    emit reviewerChangeRequested(number, newProvider);
}

void AgentWorkspaceView::acceptReviewerState(const QVariantMap& value) {
    if (reviewerScope_.isEmpty())
        return;
    auto state = value;
    // A failed change keeps showing the reviewer that is still assigned; a daemon that
    // cannot read reviewers hides the picker.
    if (state.contains("error")) {
        state.insert("available", reviewerState_.value("available", false));
        if (reviewerState_.contains("reviewer"))
            state.insert("reviewer", reviewerState_.value("reviewer"));
    }
    state.insert("pending", false);
    reviewerState_ = state;
    emit reviewerStateChanged();
}
} // namespace ketplus
