#include "agent/AgentWorkspaceView.h"

namespace ketplus {
void AgentWorkspaceView::setBackgroundTaskState(const QVariantMap& value) {
    if (value.isEmpty() || value.value("scope") != backgroundTaskState_.value("scope")) {
        backgroundTaskActions_.clear();
        closeBackgroundTaskOutput();
    }
    if (backgroundTaskState_ == value)
        return;
    backgroundTaskState_ = value;
    emit backgroundTaskStateChanged();
}

void AgentWorkspaceView::stopBackgroundTask(const QString& taskId) {
    if (!agentAvailable_ || !backgroundTaskState_.value("connected", true).toBool() ||
        !backgroundTaskState_.value("canControl", true).toBool() ||
        backgroundTaskActions_.value(taskId).toMap().value("pending").toBool())
        return;
    backgroundTaskActions_.insert(taskId, QVariantMap{{"pending", true}});
    emit backgroundTaskOutputChanged();
    emit backgroundTaskStopRequested(taskId);
}

void AgentWorkspaceView::requestBackgroundTaskOutput(const QString& taskId) {
    if (!agentAvailable_ || !backgroundTaskState_.value("connected", true).toBool() ||
        !backgroundTaskState_.value("canRead", true).toBool())
        return;
    const bool same = backgroundTaskOutput_.value("taskId").toString() == taskId;
    if (same && backgroundTaskOutput_.value("loading").toBool())
        return;
    if (!same)
        backgroundTaskOutput_ = {{"taskId", taskId}};
    const auto task = std::find_if(backgroundTasks_.cbegin(), backgroundTasks_.cend(),
                                   [&taskId](const QVariant& value) {
                                       return value.toMap().value("task_id").toString() == taskId;
                                   });
    const bool managed =
        task != backgroundTasks_.cend() && task->toMap().contains("schema_version");
    const QString cursor =
        same && managed ? backgroundTaskOutput_.value("nextCursor").toString() : QString{};
    backgroundTaskOutput_.insert("loading", true);
    backgroundTaskOutput_.insert("append", managed && !cursor.isEmpty());
    backgroundTaskOutput_.insert("request", QString::number(++backgroundTaskRequest_));
    emit backgroundTaskOutputChanged();
    emit backgroundTaskOutputRequested(taskId, cursor, backgroundTaskRequest_);
}

void AgentWorkspaceView::acceptBackgroundTaskResult(const QString& taskId, const QString& action,
                                                    const QVariantMap& result) {
    const QString error = result.value("error").toString();
    if (action != "output") {
        backgroundTaskActions_.insert(taskId, QVariantMap{{"pending", false}, {"error", error}});
        emit backgroundTaskOutputChanged();
        if (!error.isEmpty())
            emit backgroundTaskFailed(error);
        return;
    }
    if (backgroundTaskOutput_.value("taskId").toString() != taskId ||
        (!result.value("request").toString().isEmpty() &&
         result.value("request").toString() != backgroundTaskOutput_.value("request").toString()))
        return;
    backgroundTaskOutput_.insert("loading", false);
    backgroundTaskOutput_.insert("error", error);
    if (error.isEmpty()) {
        QString output = result.value("output").toString();
        const bool append = backgroundTaskOutput_.value("append").toBool();
        if (append) {
            if (result.value("truncated").toBool())
                output.prepend("\n[Output gap: earlier bytes are no longer retained]\n");
            output.prepend(backgroundTaskOutput_.value("output").toString());
        }
        // Bounded presentation; the daemon retains the spool and owns byte cursors.
        constexpr qsizetype displayCharacters = 64 * 1024;
        if (output.size() > displayCharacters) {
            output = output.right(displayCharacters);
            if (!output.isEmpty() && output.front().isLowSurrogate())
                output.removeFirst();
            backgroundTaskOutput_.insert("displayTruncated", true);
        }
        backgroundTaskOutput_.insert("output", output);
        backgroundTaskOutput_.insert("hasMore", !result.value("next_cursor").toString().isEmpty() &&
                                                    result.value("next_cursor") !=
                                                        backgroundTaskOutput_.value("nextCursor"));
        backgroundTaskOutput_.insert("nextCursor", result.value("next_cursor"));
        backgroundTaskOutput_.insert("totalBytes", result.value("total_bytes"));
        backgroundTaskOutput_.insert(
            "truncated", result.value("truncated").toBool() ||
                             (append && backgroundTaskOutput_.value("truncated").toBool()));
    }
    emit backgroundTaskOutputChanged();
}
} // namespace ketplus
