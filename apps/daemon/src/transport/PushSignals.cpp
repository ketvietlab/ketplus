#include "transport/PushSignals.h"
#include <QDateTime>
#include <QRegularExpression>

namespace ketplus::daemon::transport {
PushSignals::PushSignals(Sender sender, Enabled enabled, QObject* parent)
    : QObject(parent), sender_(std::move(sender)), enabled_(std::move(enabled)),
      clock_([] { return QDateTime::currentMSecsSinceEpoch(); }) {
    retry_.setSingleShot(true);
    retry_.setInterval(1000);
    connect(&retry_, &QTimer::timeout, this, &PushSignals::flush);
}

bool PushSignals::signalKind(const QString& eventType) {
    return eventType == "turn.completed" || eventType == "runtime.error" ||
           eventType == "turn.failed" || eventType == "approval.requested" ||
           eventType == "question.requested";
}

QJsonObject PushSignals::message(const QJsonObject& event) {
    static const QRegularExpression pattern("^[A-Za-z0-9_.:-]{1,128}$");
    const auto kind = event.value("event_type").toString();
    const auto session = event.value("session_id").toString();
    const auto cursor = event.value("cursor");
    if (!signalKind(kind) || !pattern.match(session).hasMatch() || !cursor.isDouble() ||
        cursor.toInteger() <= 0)
        return {};
    return {{"type", "push.signal"},
            {"sessionId", session},
            {"routingId", session},
            {"kind", kind},
            {"cursor", cursor.toInteger()}};
}

void PushSignals::note(const QJsonObject& event) {
    if (!enabled_ || !enabled_()) {
        queue_.clear();
        return;
    }
    const auto notice = message(event);
    if (notice.isEmpty())
        return;
    while (queue_.size() >= maxPending)
        queue_.dequeue();
    queue_.enqueue({notice, clock_()});
    flush();
}

void PushSignals::flush() {
    if (!enabled_ || !enabled_()) {
        queue_.clear();
        return;
    }
    while (!queue_.isEmpty()) {
        if (clock_() - queue_.head().at > maxAgeMs) {
            queue_.dequeue();
            continue;
        }
        if (!sender_ || !sender_(queue_.head().message)) {
            // Not connected or backed up: keep the bounded queue and look again shortly.
            if (!retry_.isActive())
                retry_.start();
            return;
        }
        queue_.dequeue();
    }
}
} // namespace ketplus::daemon::transport
