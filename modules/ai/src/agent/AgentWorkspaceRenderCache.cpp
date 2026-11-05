#include "agent/AgentMessageModel.h"
#include "agent/AgentWorkspaceView.h"

#include <QMetaObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickView>

#include <algorithm>
#include <chrono>

namespace ketplus {
namespace {

using Clock = std::chrono::steady_clock;

struct WarmTranscript final {
    QPointer<AgentWorkspaceView> owner;
    QString scope;
    Clock::time_point lastUsed;
};

QList<WarmTranscript>& warmTranscripts() {
    static QList<WarmTranscript> entries;
    return entries;
}

constexpr qsizetype MaximumWarmTranscripts = 8;
constexpr auto WarmTranscriptLifetime = std::chrono::minutes(5);

} // namespace

void AgentWorkspaceView::touchRenderTranscript(const QString& scope) {
    activeRenderScope_ = scope;
    pruneRenderTranscripts();

    auto& entries = warmTranscripts();
    entries.removeIf([this, &scope](const WarmTranscript& entry) {
        return entry.owner == this && entry.scope == scope;
    });
    entries.append({this, scope, Clock::now()});

    // Models cache their byte estimate until mutated; sum once per touch and
    // subtract evictions instead of re-walking every warm model per iteration.
    const auto modelBytes = [](const WarmTranscript& entry) -> qsizetype {
        if (!entry.owner)
            return 0;
        const auto model = entry.owner->messageModels_.value(entry.scope);
        return model ? model->estimatedTextBytes() : 0;
    };
    qsizetype retainedTextBytes = 0;
    for (const auto& entry : entries)
        retainedTextBytes += modelBytes(entry);
    while (entries.size() > MaximumWarmTranscripts || retainedTextBytes > 16 * 1024 * 1024) {
        const auto victim =
            std::find_if(entries.begin(), entries.end(), [](const WarmTranscript& entry) {
                return entry.owner && entry.owner->activeRenderScope_ != entry.scope;
            });
        if (victim == entries.end())
            break;
        const WarmTranscript evicted = *victim;
        retainedTextBytes -= modelBytes(evicted);
        entries.erase(victim);
        if (evicted.owner)
            evicted.owner->releaseRenderTranscript(evicted.scope);
    }
}

void AgentWorkspaceView::pruneRenderTranscripts() {
    const auto now = Clock::now();
    auto& entries = warmTranscripts();
    for (auto iterator = entries.begin(); iterator != entries.end();) {
        const bool expired =
            !iterator->owner || (iterator->owner->activeRenderScope_ != iterator->scope &&
                                 now - iterator->lastUsed >= WarmTranscriptLifetime);
        if (!expired) {
            ++iterator;
            continue;
        }
        const WarmTranscript released = *iterator;
        iterator = entries.erase(iterator);
        if (released.owner)
            released.owner->releaseRenderTranscript(released.scope);
    }
}

void AgentWorkspaceView::removeRenderTranscripts(AgentWorkspaceView* owner) {
    auto& entries = warmTranscripts();
    entries.removeIf([owner](const WarmTranscript& entry) { return entry.owner == owner; });
}

void AgentWorkspaceView::releaseRenderTranscript(const QString& scope) {
    if (!view_ || !view_->rootObject() || activeRenderScope_ == scope)
        return;
    QMetaObject::invokeMethod(view_->rootObject(), "releaseSessionTranscript",
                              Q_ARG(QVariant, scope));
    if (auto model = messageModels_.take(scope))
        model->deleteLater();
}

int AgentWorkspaceView::benchmarkWarmTranscriptCount() const {
    if (!view_ || !view_->rootObject())
        return 0;
    QVariant count;
    QMetaObject::invokeMethod(view_->rootObject(), "warmTranscriptCount",
                              Q_RETURN_ARG(QVariant, count));
    return count.toInt();
}

bool AgentWorkspaceView::benchmarkUsableViewport() const {
    if (!view_ || !view_->rootObject())
        return false;
    QList<QQuickItem*> pending{view_->rootObject()};
    while (!pending.isEmpty()) {
        auto* item = pending.takeLast();
        pending.append(item->childItems());
        if (item->objectName() == QStringLiteral("messageBody") &&
            item->property("renderInViewport").toBool() &&
            item->property("sourceInitialized").toBool() &&
            !item->property("text").toString().isEmpty())
            return true;
    }
    return false;
}

} // namespace ketplus
