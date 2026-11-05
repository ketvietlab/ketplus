#include "DaemonClient.h"
#include "LargeEventReassembler.h"
#include "agent/CodexAppServer.h"

#include <QJsonArray>
#include <algorithm>
#include <memory>
#include <utility>

namespace ketplus {
namespace {

bool validWindow(const QJsonObject& page, const QString& sessionId, qint64 generation,
                 const qint64 afterCursor) {
    if (afterCursor > 0 &&
        (!page.value("has_more_after").isBool() || !page.value("after_cursor").isDouble()))
        return false;
    if (page.value("transcript_window_version").toInt() != 1 ||
        page.value("session_id").toString() != sessionId ||
        page.value("session_generation").toInteger() != generation ||
        !page.value("events").isArray() || !page.value("snapshot_cursor").isDouble() ||
        !page.value("projection_revision").isDouble() || !page.value("has_more_before").isBool())
        return false;
    const auto events = page.value("events").toArray();
    const auto snapshot = page.value("snapshot_cursor").toInteger(-1);
    if (snapshot < 0 || page.value("projection_revision").toInteger(-1) < 0 || events.size() > 128)
        return false;
    qint64 previous = 0;
    for (const auto& value : events) {
        const auto event = value.toObject();
        const auto cursor = event.value("cursor").toInteger(-1);
        if (event.value("session_id").toString() != sessionId ||
            event.value("session_generation").toInteger() != generation || cursor <= previous ||
            cursor > snapshot || !event.value("payload").isObject() ||
            event.value("turn_sequence").toInteger(-1) < 0 || cursor <= afterCursor)
            return false;
        previous = cursor;
    }
    if (!events.isEmpty() && page.value("before_cursor").toInteger() !=
                                 events.first().toObject().value("cursor").toInteger())
        return false;
    if (afterCursor > 0) {
        if (!events.isEmpty() && page.value("after_cursor").toInteger() !=
                                     events.last().toObject().value("cursor").toInteger())
            return false;
        return !page.value("has_more_after").toBool() || !events.isEmpty();
    }
    return !page.value("has_more_before").toBool() || !events.isEmpty();
}

} // namespace

bool CodexAppServer::isDaemonTranscriptWindowCurrent(const QString& sessionId) const {
    const auto found = daemonSessions_.constFind(sessionId);
    return daemon_ && daemonHasTranscriptWindow_ && found != daemonSessions_.cend() &&
           found->windowReady && !found->replaying;
}

QJsonObject CodexAppServer::beginDaemonTranscriptCatchup(const QString& sessionId) {
    auto found = daemonSessions_.find(sessionId);
    if (!daemon_ || !hasDaemonTranscriptAfterCursor() || found == daemonSessions_.end() ||
        found->replaying || found->windowCursor <= 0)
        return {};
    ++found->windowEpoch;
    found->replaying = true;
    found->windowReady = false;
    found->windowOverflow = false;
    found->approvalSyncInFlight = true;
    found->settledApprovals.clear();
    found->deferredEvents.clear();
    found->deferredBytes = 0;
    found->deferredCursor = found->cursor;
    return {{"after_cursor", found->windowCursor}, {"snapshot_cursor", found->cursor}};
}

void CodexAppServer::cancelDaemonTranscriptCatchup(const QString& sessionId) {
    auto found = daemonSessions_.find(sessionId);
    if (found == daemonSessions_.end())
        return;
    ++found->windowEpoch;
    // The next selection must include events received during the canceled load,
    // including events that exceeded the bounded live buffer.
    found->cursor = std::max(found->cursor, found->deferredCursor);
    found->replaying = false;
    found->windowReady = false;
    found->approvalSyncInFlight = false;
    found->settledApprovals.clear();
    found->deferredEvents.clear();
    found->deferredBytes = 0;
}

void CodexAppServer::finishDaemonTranscriptCatchup(const QString& sessionId,
                                                   const QJsonObject& page,
                                                   ResponseCallback callback) {
    auto found = daemonSessions_.find(sessionId);
    if (found == daemonSessions_.end() || !found->replaying || found->windowOverflow) {
        cancelDaemonTranscriptCatchup(sessionId);
        callback({},
                 QStringLiteral("Conversation changed too quickly while loading. Retry history."));
        return;
    }
    finishDaemonTranscriptWindow(sessionId, found->generation, found->windowEpoch, true, page, {},
                                 std::move(callback));
}

void CodexAppServer::readDaemonTranscriptWindow(const QString& sessionId, const QJsonObject& params,
                                                ResponseCallback callback,
                                                const bool preserveLiveBuffer) {
    auto found = daemonSessions_.find(sessionId);
    // Three unrelated refusals used to share the capability message, which sent a
    // reader chasing a daemon upgrade for what is usually a session this process
    // has not registered yet.
    if (!daemon_) {
        callback({}, QStringLiteral("The KetPlus daemon is not connected."));
        return;
    }
    if (!daemonHasTranscriptWindow_) {
        callback({}, QStringLiteral("This daemon does not support bounded transcript windows."));
        return;
    }
    if (found == daemonSessions_.end()) {
        callback({}, QStringLiteral("This conversation is not attached to the daemon yet."));
        return;
    }
    const qint64 afterCursor = params.value("after_cursor").toInteger(0);
    if (preserveLiveBuffer && !found->replaying) {
        callback({}, QStringLiteral("Transcript catch-up is no longer active."));
        return;
    }
    const bool initial = params.value("before_cursor").toInteger() == 0 && afterCursor == 0 &&
                         !preserveLiveBuffer && !params.value("raw_activity_details").toBool();
    if (afterCursor > 0 && !daemonHasTranscriptAfterCursor_) {
        callback({}, QStringLiteral("This daemon does not support newer transcript pages."));
        return;
    }
    const auto generation = found->generation;
    const auto epoch = initial ? ++found->windowEpoch : found->windowEpoch;
    if (initial) {
        found->replaying = true;
        found->windowReady = false;
        found->windowOverflow = false;
        found->approvalSyncInFlight = true;
        found->settledApprovals.clear();
        found->deferredEvents.clear();
        found->deferredBytes = 0;
        found->deferredCursor = found->cursor;
    }
    daemon_->call(
        "session.transcript_window", params, sessionId, generation,
        [this, sessionId, generation, epoch, initial, afterCursor,
         callback = std::move(callback)](const QJsonObject& result, const QString& error) mutable {
            auto found = daemonSessions_.find(sessionId);
            if (found == daemonSessions_.end() || found->generation != generation ||
                found->windowEpoch != epoch) {
                callback({}, QStringLiteral("The conversation changed while loading history."));
                return;
            }
            QString failure = error;
            if (failure.isEmpty() && !validWindow(result, sessionId, generation, afterCursor))
                failure = QStringLiteral("Invalid daemon transcript window.");
            if (!failure.isEmpty()) {
                finishDaemonTranscriptWindow(sessionId, generation, epoch, initial, result, failure,
                                             std::move(callback));
                return;
            }
            resolveDaemonLargeEvents(
                sessionId, generation, epoch, result,
                [this, sessionId, generation, epoch, initial, callback = std::move(callback)](
                    const QJsonObject& resolved, const QString& resolveError) mutable {
                    finishDaemonTranscriptWindow(sessionId, generation, epoch, initial, resolved,
                                                 resolveError, std::move(callback));
                });
        });
}

void CodexAppServer::resolveDaemonLargeEvents(const QString& sessionId, const qint64 generation,
                                              const quint64 epoch, const QJsonObject& page,
                                              ResponseCallback callback) {
    struct State final {
        QJsonObject page;
        QJsonArray events;
        qsizetype nextIndex{0};
        int active{0};
        bool finished{false};
        ResponseCallback callback;
    };
    auto state = std::make_shared<State>();
    state->page = page;
    state->events = page.value("events").toArray();
    state->callback = std::move(callback);
    const auto finish = [state](const QJsonObject& result, const QString& error) {
        if (std::exchange(state->finished, true))
            return;
        auto callback = std::move(state->callback);
        callback(result, error);
    };
    // Large items each need a serial fragment stream, but independent items in
    // the same window can be fetched together. Keep the fan-out below the local
    // transport's in-flight limit so other daemon work can still make progress.
    constexpr int maximumConcurrentItems = 4;
    auto advance = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakAdvance = advance;
    *advance = [this, sessionId, generation, epoch, state, weakAdvance, finish]() {
        const auto continuation = weakAdvance.lock();
        if (!continuation || state->finished)
            return;
        auto session = daemonSessions_.find(sessionId);
        if (session == daemonSessions_.end() || session->generation != generation ||
            session->windowEpoch != epoch) {
            finish({}, QStringLiteral("The conversation changed while loading large content."));
            return;
        }
        while (state->active < maximumConcurrentItems && state->nextIndex < state->events.size()) {
            const qsizetype index = state->nextIndex++;
            const QJsonObject event = state->events.at(index).toObject();
            const QJsonObject reference =
                event.value("payload").toObject().value("large_event").toObject();
            if (reference.isEmpty())
                continue;
            if (!daemonHasLargeEvents_) {
                finish({}, QStringLiteral("Unsupported large transcript item."));
                return;
            }
            auto assembler = std::make_shared<remote::LargeEventReassembler>(reference);
            if (!assembler->isValid() ||
                assembler->eventType() != event.value("event_type").toString()) {
                finish({}, QStringLiteral("Invalid large transcript reference."));
                return;
            }
            ++state->active;
            auto fetch = std::make_shared<std::function<void()>>();
            std::weak_ptr<std::function<void()>> weakFetch = fetch;
            *fetch = [this, sessionId, generation, epoch, state, continuation, weakFetch, assembler,
                      index, finish]() {
                const auto nextFragment = weakFetch.lock();
                if (!nextFragment || state->finished)
                    return;
                daemon_->call(
                    "session.transcript_large_event",
                    {{"transfer_id", assembler->transferId()},
                     {"offset", assembler->nextOffset()},
                     {"maximum_bytes", 128 * 1024}},
                    sessionId, generation,
                    [this, sessionId, generation, epoch, state, continuation, nextFragment,
                     assembler, index,
                     finish](const QJsonObject& fragment, const QString& fragmentError) {
                        if (state->finished)
                            return;
                        auto session = daemonSessions_.find(sessionId);
                        if (session == daemonSessions_.end() || session->generation != generation ||
                            session->windowEpoch != epoch) {
                            finish({},
                                   QStringLiteral(
                                       "The conversation changed while loading large content."));
                            return;
                        }
                        QString validationError = fragmentError;
                        if (validationError.isEmpty() &&
                            (fragment.value("large_event_fragment_version").toInt() != 1 ||
                             fragment.value("session_id").toString() != sessionId ||
                             fragment.value("session_generation").toInteger() != generation ||
                             !assembler->accept(fragment, &validationError))) {
                            if (validationError.isEmpty())
                                validationError = QStringLiteral(
                                    "Conflicting or incomplete large transcript fragment.");
                        }
                        if (!validationError.isEmpty()) {
                            finish({}, validationError);
                            return;
                        }
                        if (!assembler->isComplete()) {
                            (*nextFragment)();
                            return;
                        }
                        const QJsonObject payload = assembler->payload(&validationError);
                        if (payload.isEmpty()) {
                            finish({}, validationError);
                            return;
                        }
                        QJsonObject event = state->events.at(index).toObject();
                        event.insert("payload", payload);
                        state->events.replace(index, event);
                        --state->active;
                        (*continuation)();
                    });
            };
            (*fetch)();
        }
        if (state->nextIndex < state->events.size() || state->active > 0)
            return;
        state->page.insert("events", state->events);
        QJsonArray retainedPartials;
        for (const auto& value : state->page.value("partial_items").toArray()) {
            if (value.toObject().value("transfer_id").toString().isEmpty())
                retainedPartials.append(value);
        }
        state->page.insert("partial_items", retainedPartials);
        finish(state->page, {});
    };
    (*advance)();
}

void CodexAppServer::finishDaemonTranscriptWindow(const QString& sessionId, const qint64 generation,
                                                  const quint64 epoch, const bool initial,
                                                  const QJsonObject& result, const QString& error,
                                                  ResponseCallback callback) {
    auto found = daemonSessions_.find(sessionId);
    if (found == daemonSessions_.end() || found->generation != generation ||
        found->windowEpoch != epoch) {
        callback({}, QStringLiteral("The conversation changed while loading history."));
        return;
    }
    QString failure = error;
    if (initial && found->windowOverflow)
        failure = QStringLiteral("Conversation changed too quickly while loading. Retry history.");
    if (!initial) {
        callback(result, failure);
        return;
    }
    found->replaying = false;
    const auto deferred = std::exchange(found->deferredEvents, {});
    found->deferredBytes = 0;
    if (!failure.isEmpty()) {
        found->approvalSyncInFlight = false;
        found->settledApprovals.clear();
        callback(result, failure);
        for (const auto& event : deferred)
            dispatchDaemonEvent(event);
        return;
    }
    found->cursor = result.value("snapshot_cursor").toInteger();
    found->windowCursor = found->cursor;
    found->turnId = result.value("active_turn_id").toString();
    found->windowReady = true;
    callback(result, {});
    found = daemonSessions_.find(sessionId);
    if (found == daemonSessions_.end() || found->windowEpoch != epoch)
        return;
    for (const auto& value : result.value("pending_approvals").toArray())
        dispatchDaemonApproval(sessionId, value.toObject());
    for (const auto& event : deferred)
        dispatchDaemonEvent(event);
    if (result.value("pending_approvals_has_more").toBool()) {
        auto next = std::make_shared<std::function<void(QString)>>();
        std::weak_ptr<std::function<void(QString)>> weak = next;
        *next = [this, sessionId, generation, epoch, weak](QString after) {
            const auto continuation = weak.lock();
            if (!continuation)
                return;
            daemon_->call("session.pending_approvals", {{"after_id", after}}, sessionId, generation,
                          [this, sessionId, epoch, after, continuation](const QJsonObject& page,
                                                                        const QString& pageError) {
                              auto found = daemonSessions_.find(sessionId);
                              if (found == daemonSessions_.end() || found->windowEpoch != epoch)
                                  return;
                              if (!pageError.isEmpty()) {
                                  found->approvalSyncInFlight = false;
                                  found->settledApprovals.clear();
                                  emit notificationReceived(
                                      "daemon/approvalSyncFailed",
                                      {{"threadId", sessionId}, {"message", pageError}});
                                  return;
                              }
                              for (const auto& value : page.value("pending_approvals").toArray())
                                  dispatchDaemonApproval(sessionId, value.toObject());
                              const auto cursor =
                                  page.value("pending_approvals_next_id").toString();
                              if (page.value("pending_approvals_has_more").toBool() &&
                                  !cursor.isEmpty() && cursor != after)
                                  (*continuation)(cursor);
                              else {
                                  found->approvalSyncInFlight = false;
                                  found->settledApprovals.clear();
                              }
                          });
        };
        (*next)(result.value("pending_approvals_next_id").toString());
    } else {
        found = daemonSessions_.find(sessionId);
        if (found != daemonSessions_.end()) {
            found->approvalSyncInFlight = false;
            found->settledApprovals.clear();
        }
    }
}

} // namespace ketplus
