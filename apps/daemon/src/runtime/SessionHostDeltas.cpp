#include "runtime/SessionHost.h"

namespace ketplus::daemon {
namespace {

bool isStreamDelta(const QString& type) {
    return type == QStringLiteral("assistant.delta") ||
           type == QStringLiteral("command.output_delta");
}

// Two deltas continue one stream when everything but their text matches: the same item from the
// same provider.
bool continuesDelta(QJsonObject held, QJsonObject next) {
    held.remove(QStringLiteral("delta"));
    next.remove(QStringLiteral("delta"));
    return held == next;
}

} // namespace

SessionHost::~SessionHost() {
    // Text held back for coalescing is journaled rather than lost with the daemon.
    deltaFlush_.stop();
    flushDeltas();
}

bool SessionHost::appendEvent(Session& session, const QString& type, const QJsonObject& payload,
                              QString* error) {
    if (!isStreamDelta(type)) {
        // Journal what the provider streamed before whatever followed it.
        (void)flushDelta(session);
        return journalEvent(session, type, payload, error);
    }
    if (!session.pendingDeltaType.isEmpty() &&
        (session.pendingDeltaType != type || !continuesDelta(session.pendingDelta, payload)))
        (void)flushDelta(session);
    if (session.pendingDeltaType.isEmpty()) {
        session.pendingDeltaType = type;
        session.pendingDelta = payload;
    } else {
        session.pendingDelta.insert(QStringLiteral("delta"),
                                    session.pendingDelta.value(QStringLiteral("delta")).toString() +
                                        payload.value(QStringLiteral("delta")).toString());
    }
    if (session.pendingDelta.value(QStringLiteral("delta")).toString().size() >=
        maximumCoalescedDeltaBytes)
        return flushDelta(session, error);
    if (!deltaFlush_.isActive())
        deltaFlush_.start();
    return true;
}

bool SessionHost::flushDelta(Session& session, QString* error) {
    if (session.pendingDeltaType.isEmpty())
        return true;
    const QString type = std::exchange(session.pendingDeltaType, {});
    const QJsonObject payload = std::exchange(session.pendingDelta, {});
    return journalEvent(session, type, payload, error);
}

void SessionHost::flushDeltas() {
    for (Session& session : sessions_)
        (void)flushDelta(session);
}

} // namespace ketplus::daemon
