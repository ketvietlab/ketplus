#include "protocol/DaemonProtocol.h"
#include "runtime/SessionHost.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <limits>

namespace ketplus::daemon {
namespace {
constexpr qsizetype maximumDeferredEventsPerSession = 128;
constexpr qsizetype maximumDeferredEventsTotal = 512;
constexpr qsizetype maximumDeferredBytesPerSession = 40 * 1024 * 1024;
constexpr qsizetype maximumDeferredBytesTotal = 80 * 1024 * 1024;
constexpr int journalRetryMs = 1000;
constexpr qsizetype maximumCompletedItems = 512;

bool isStreamDelta(const QString& type) {
    return type == QStringLiteral("assistant.delta") ||
           type == QStringLiteral("command.output_delta");
}

bool requiresDurableAcceptance(const QString& type) {
    return type == QStringLiteral("user.message") || type == QStringLiteral("turn.queued") ||
           type == QStringLiteral("session.created") ||
           type == QStringLiteral("session.provider_changed");
}

bool continuesDelta(QJsonObject held, QJsonObject next) {
    held.remove(QStringLiteral("delta"));
    next.remove(QStringLiteral("delta"));
    return held == next;
}

void scheduleFlush(QTimer& timer, int delay) {
    if (!timer.isActive() || timer.remainingTime() > delay)
        timer.start(delay);
}
} // namespace

SessionHost::~SessionHost() {
    deltaFlush_.stop();
    // Questions to git nobody will read: the ones not started are dropped; the pool's own
    // destructor waits for the one that runs.
    teamGit_.clear();
    teamBranchNames_.clear();
    teamRemote_.clear();
    teamRemoteStopping_->store(true);
    // The journal outlives this host and its media store.
    journal_->setImagePreviewWriter({});
    // Drain accepted in-memory data while storage is available. A failed write
    // stops this session's drain; shutdown must not spin on an unavailable disk.
    const auto ids = sessions_.keys();
    for (const auto& id : ids) {
        for (qsizetype attempt = 0; attempt <= maximumDeferredEventsPerSession; ++attempt) {
            auto it = sessions_.find(id);
            if (it == sessions_.end() ||
                (it->pendingDeltaType.isEmpty() && it->deferredJournal.isEmpty()))
                break;
            it->journalRetryAt = QDeadlineTimer(0);
            if (!flushDelta(*it))
                break;
        }
    }
    deltaFlush_.stop();
}

bool SessionHost::deferJournalEvent(Session& session, const QString& type,
                                    const QJsonObject& payload, QString* error) {
    QJsonObject held = payload;
    bool merge = false;
    qsizetype oldBytes = 0;
    if (!session.deferredJournal.isEmpty() && isStreamDelta(type)) {
        const auto& last = session.deferredJournal.last();
        const QString text =
            last.payload.value("delta").toString() + payload.value("delta").toString();
        if (last.type == type && continuesDelta(last.payload, payload) &&
            text.size() <= maximumCoalescedDeltaBytes) {
            merge = true;
            held.insert("delta", text);
            oldBytes = last.bytes;
        }
    }
    const qsizetype encodedBytes = QJsonDocument(held).toJson(QJsonDocument::Compact).size();
    const qsizetype bytes = encodedBytes + type.toUtf8().size() + 512;
    const qsizetype added = bytes - oldBytes;
    const auto item = held.value(QStringLiteral("item")).toObject();
    const auto itemType =
        held.value(QStringLiteral("item_type")).toString(item.value("type").toString());
    const auto itemId = held.value(QStringLiteral("item_id")).toString();
    const bool chunkable =
        (type == QStringLiteral("item.started") || type == QStringLiteral("item.completed")) &&
        itemType != QStringLiteral("commandExecution") &&
        (!itemId.isEmpty() || !item.value("id").toString().isEmpty());
    // Match the journal's large-item lane. A permanently unrepresentable event
    // must not pin the retry head and block otherwise valid later events.
    if (encodedBytes > 32 * 1024 * 1024 ||
        (encodedBytes > protocol::maximumEventPayloadBytes && !chunkable) ||
        session.deferredJournalBytes + added > maximumDeferredBytesPerSession ||
        deferredJournalBytes_ + added > maximumDeferredBytesTotal ||
        (!merge && (session.deferredJournal.size() >= maximumDeferredEventsPerSession ||
                    deferredJournalCount_ >= maximumDeferredEventsTotal))) {
        const QString message =
            QStringLiteral("The journal recovery buffer cannot retain this event.");
        if (error)
            *error = message;
        if (!session.journalBufferFullReported) {
            qWarning("Unable to retain journal event %s for session %s: recovery limit reached",
                     qUtf8Printable(type), qUtf8Printable(session.stored.id));
            session.journalBufferFullReported = true;
        }
        return false;
    }
    if (session.deferredJournal.isEmpty())
        session.journalRetryAt.setRemainingTime(journalRetryMs);
    if (merge)
        session.deferredJournal.last() = {type, held, bytes};
    else {
        session.deferredJournal.append({type, held, bytes});
        ++deferredJournalCount_;
    }
    session.deferredJournalBytes += added;
    deferredJournalBytes_ += added;
    scheduleFlush(deltaFlush_, int(qMax<qint64>(0, session.journalRetryAt.remainingTime())));
    return true;
}

bool SessionHost::appendEvent(Session& session, const QString& type,
                              const QJsonObject& providerPayload, QString* error) {
    // A turn's end tells readiness how the provider is doing (#170); errors gain a code.
    const QJsonObject payload = noteProviderOutcome(session, type, providerPayload);
    noteAwaitingReply(session, type, payload);
    const QString id = session.stored.id;
    const qint64 generation = session.stored.generation;
    const QPointer<SessionHost> alive(this);
    const auto current = [&]() -> Session* {
        if (alive.isNull())
            return nullptr;
        auto it = sessions_.find(id);
        return it != sessions_.end() && it->stored.generation == generation ? &*it : nullptr;
    };
    // Whether the event was committed or queued for retry; one dropped entirely leaves nothing
    // for clients to see, so it does not mark its item complete.
    bool kept = false;
    const auto retain = [&](Session& target) {
        if (error && error->isEmpty())
            *error = QStringLiteral("The local journal has events awaiting recovery.");
        // A rejected user command must never become a queued ghost prompt.
        if (requiresDurableAcceptance(type))
            return false;
        if (!target.pendingDeltaType.isEmpty()) {
            if (!deferJournalEvent(target, target.pendingDeltaType, target.pendingDelta, error))
                return false;
            target.pendingDeltaType.clear();
            target.pendingDelta = {};
        }
        kept = deferJournalEvent(target, type, payload, error);
        return false; // Retained is not the same as committed.
    };
    // Ids are provider strings of any length; a fixed-size digest bounds what is remembered.
    const QString itemId = payload.value(QStringLiteral("item_id")).toString();
    const QByteArray itemKey =
        itemId.isEmpty() ? QByteArray{}
                         : QCryptographicHash::hash(itemId.toUtf8(), QCryptographicHash::Sha256);
    if (!itemKey.isEmpty()) {
        // A provider stream is ordered, but a delta that arrives after its item completed (a
        // retried notification, or the tail of a previous turn) would extend text that is
        // already final, live and in every replay. The completed item is authoritative.
        if (isStreamDelta(type) && session.completedItems.contains(itemKey))
            return true;
        if (type == QStringLiteral("item.started") && session.completedItems.remove(itemKey))
            session.completedItemOrder.removeOne(itemKey);
    }
    const auto rememberCompletion = qScopeGuard([&] {
        if (!kept || type != QStringLiteral("item.completed") || itemKey.isEmpty())
            return;
        auto* target = current();
        if (target == nullptr || target->completedItems.contains(itemKey))
            return;
        target->completedItems.insert(itemKey);
        target->completedItemOrder.append(itemKey);
        if (target->completedItemOrder.size() > maximumCompletedItems)
            target->completedItems.remove(target->completedItemOrder.takeFirst());
    });
    if (!session.deferredJournal.isEmpty())
        return retain(session);
    if (!isStreamDelta(type)) {
        if (!flushDelta(session, error))
            return retain(session);
        auto* active = current();
        if (!active)
            return false;
        if (journalEvent(*active, type, payload, error)) {
            kept = true;
            return true;
        }
        return retain(*active);
    }
    const bool same =
        session.pendingDeltaType == type && continuesDelta(session.pendingDelta, payload);
    const bool fits = session.pendingDelta.value("delta").toString().size() +
                          payload.value("delta").toString().size() <=
                      maximumCoalescedDeltaBytes;
    if (!session.pendingDeltaType.isEmpty() && (!same || !fits)) {
        if (!flushDelta(session, error))
            return retain(session);
    }
    auto* active = current();
    if (!active)
        return false;
    if (active->pendingDeltaType.isEmpty()) {
        active->pendingDeltaType = type;
        active->pendingDelta = payload;
    } else {
        active->pendingDelta.insert("delta", active->pendingDelta.value("delta").toString() +
                                                 payload.value("delta").toString());
    }
    if (active->pendingDelta.value("delta").toString().size() >= maximumCoalescedDeltaBytes)
        return flushDelta(*active, error);
    scheduleFlush(deltaFlush_, deltaCoalesceMs);
    return true;
}

bool SessionHost::flushDelta(Session& session, QString* error) {
    if (session.pendingDeltaType.isEmpty() && session.deferredJournal.isEmpty())
        return true;
    if (!session.journalRetryAt.hasExpired()) {
        if (error)
            *error = QStringLiteral("The local journal has events awaiting recovery.");
        return false;
    }
    if (!session.deferredJournal.isEmpty()) {
        const auto event = session.deferredJournal.takeFirst();
        session.deferredJournalBytes -= event.bytes;
        deferredJournalBytes_ -= event.bytes;
        --deferredJournalCount_;
        const bool wasFull = session.journalBufferFullReported;
        if (session.deferredJournal.isEmpty())
            session.journalBufferFullReported = false;
        else
            scheduleFlush(deltaFlush_, 0);
        // Detach before emission: a synchronous subscriber can delete the session.
        if (journalEvent(session, event.type, event.payload, error))
            return true;
        session.deferredJournal.prepend(event);
        session.deferredJournalBytes += event.bytes;
        deferredJournalBytes_ += event.bytes;
        ++deferredJournalCount_;
        session.journalBufferFullReported = wasFull;
        session.journalRetryAt.setRemainingTime(journalRetryMs);
        scheduleFlush(deltaFlush_, journalRetryMs);
        return false;
    }
    const QString type = std::exchange(session.pendingDeltaType, {});
    const QJsonObject payload = std::exchange(session.pendingDelta, {});
    if (journalEvent(session, type, payload, error))
        return true;
    if (!deferJournalEvent(session, type, payload, error) &&
        QJsonDocument(payload).toJson(QJsonDocument::Compact).size() <=
            protocol::maximumEventPayloadBytes) {
        // A full shared recovery budget must not erase the already-held prefix.
        session.pendingDeltaType = type;
        session.pendingDelta = payload;
        session.journalRetryAt.setRemainingTime(journalRetryMs);
        scheduleFlush(deltaFlush_, journalRetryMs);
    }
    return false;
}

void SessionHost::flushDeltas() {
    const QPointer<SessionHost> alive(this);
    const auto ids = sessions_.keys();
    for (const auto& id : ids) {
        auto it = sessions_.find(id);
        if (it != sessions_.end() && it->journalRetryAt.hasExpired())
            (void)flushDelta(*it);
        if (alive.isNull())
            return;
    }
    int delay = std::numeric_limits<int>::max();
    for (const auto& session : sessions_) {
        if (!session.deferredJournal.isEmpty() || !session.pendingDeltaType.isEmpty()) {
            const qint64 remaining = session.journalRetryAt.remainingTime();
            const int next = remaining > 0                        ? int(remaining)
                             : !session.deferredJournal.isEmpty() ? 0
                                                                  : deltaCoalesceMs;
            delay = qMin(delay, next);
        }
    }
    if (delay != std::numeric_limits<int>::max())
        scheduleFlush(deltaFlush_, delay);
}

} // namespace ketplus::daemon
