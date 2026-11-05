#include "agent/AgentWorkspaceView.h"

#include <QQuickItem>
#include <QQuickView>
#include <QTimer>

#include <utility>

namespace ketplus {

void AgentWorkspaceView::updateMessage(const int index, const QVariantMap& message) {
    if (!view_->rootObject() || !activeMessageModel_ || index < 0 ||
        index >= activeMessageModel_->sourceCount())
        return;

    // The session manager remains authoritative and receives every delta. Only
    // presentation snapshots are coalesced, never provider events or final text.
    const bool historyPrefixChanged =
        message.value("bodyPrefixCharacters").toLongLong() !=
        activeMessageModel_->source(index).value("bodyPrefixCharacters").toLongLong();
    const bool streaming = message.value("kind") == "assistant" && !historyPrefixChanged &&
                           (message.value("meta").toString().endsWith(QStringLiteral(" · streaming")) ||
                            message.value("partialBody").toBool());
    if (streaming) {
        pendingStreamingMessages_.insert(index, message);
        if (!streamUpdateTimer_->isActive()) {
            const auto length = message.value("body").toString().size();
            // Bound whole-document Markdown work, including unusually large
            // items. Do not restart the timer per delta: continuous streams
            // must keep making visible progress.
            streamUpdateTimer_->start(length > 256 * 1024 ? 500 : length > 32 * 1024 ? 250 : 100);
        }
        return;
    }

    // Completion/replacement is authoritative; an older queued prefix must not
    // overwrite it later. Other pending rows retain their chronological order.
    pendingStreamingMessages_.remove(index);
    flushStreamingMessages();
    QMetaObject::invokeMethod(view_->rootObject(), "updateSessionMessage", Q_ARG(QVariant, index),
                              Q_ARG(QVariant, QVariant::fromValue(message)));
}

void AgentWorkspaceView::flushStreamingMessages() {
    streamUpdateTimer_->stop();
    if (pendingStreamingMessages_.isEmpty())
        return;
    const auto pending = std::exchange(pendingStreamingMessages_, {});
    QVariantList updates;
    updates.reserve(pending.size());
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
        updates.append(QVariantMap{{"index", it.key()}, {"message", it.value()}});
    if (view_->rootObject())
        QMetaObject::invokeMethod(view_->rootObject(), "updateSessionMessages",
                                  Q_ARG(QVariant, updates));
}

} // namespace ketplus
