#include "runtime/ApprovalBroker.h"
#include "runtime/SessionHost.h"

#include <QPointer>

namespace ketplus::daemon {

bool SessionHost::journalTurnStart(Session& session, const QJsonObject& prompt,
                                   const QJsonObject& configuration, QString* error) {
    const QString id = session.stored.id;
    const qint64 generation = session.stored.generation;
    const QPointer<SessionHost> alive(this);
    if (!session.deferredJournal.isEmpty()) {
        if (error)
            *error = QStringLiteral("The local journal has events awaiting recovery.");
        return false;
    }
    if (!flushDelta(session, error) || alive.isNull())
        return false;
    auto current = sessions_.find(id);
    if (current == sessions_.end() || current->stored.generation != generation)
        return false;
    const auto events = journal_->appendTurnStart(current->stored, prompt, configuration, error);
    if (events.isEmpty())
        return false;
    current->lastJournalError.clear();
    current->submittedContextRevision = current->stored.contextRevision;
    current->stored.lastCursor = events.last().toObject().value("cursor").toVariant().toLongLong();
    ++current->stored.currentTurnSequence;
    current->stored.lastTurnStartedAt = events.first().toObject().value("sent_at").toString();
    // The database and in-memory acceptance state are complete before the first
    // signal. Defer question expiry / agent callbacks until all receipt events
    // have been published, so their new journal events follow the accepted pair.
    for (const auto& event : events) {
        emit eventReady(event.toObject());
        if (alive.isNull())
            return true;
        current = sessions_.find(id);
        if (current == sessions_.end() || current->stored.generation != generation)
            return true;
    }
    approvals_->expireAssistantQuestions(id, generation);
    if (alive.isNull())
        return true;
    for (const auto& event : events) {
        current = sessions_.find(id);
        if (current == sessions_.end() || current->stored.generation != generation)
            return true;
        const auto object = event.toObject();
        if (agents_)
            agents_->noteEvent(id, generation, object.value("event_type").toString(),
                               object.value("payload").toObject());
        if (alive.isNull())
            return true;
        // ADR 0112 §7: the leader's digest waits for the end of its turn.
        if (const QString type = object.value("event_type").toString();
            teams_ &&
            (type == QStringLiteral("turn.completed") || type == QStringLiteral("runtime.stopped")))
            teams_->memberTurnEnded(id);
        if (alive.isNull())
            return true;
    }
    return true;
}

} // namespace ketplus::daemon
