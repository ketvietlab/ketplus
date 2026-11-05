#include "agent/AgentWorkspaceView.h"
#include <QJsonObject>
#include <QPointer>
namespace ketplus {
void AgentWorkspaceView::requestClaudeAccountUsage() {
    if (claudeAccountUsage_.value("loading").toBool())
        return;
    claudeAccountUsage_ = {{"loading", true}};
    emit claudeAccountUsageChanged();
    if (!claudeUsageRequester_) {
        claudeAccountUsage_ = {
            {"error", tr("Connect to the updated local daemon to load organization usage.")}};
        emit claudeAccountUsageChanged();
        return;
    }
    const QPointer<AgentWorkspaceView> guard(this);
    claudeUsageRequester_([this, guard](const QJsonObject& result, const QString& error) {
        if (!guard)
            return;
        claudeAccountUsage_ =
            error.isEmpty() ? result.toVariantMap() : QVariantMap{{"error", error}};
        emit claudeAccountUsageChanged();
    });
}
} // namespace ketplus
