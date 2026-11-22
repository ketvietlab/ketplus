// T-1: how a conversation leaves its native thread. A provider handoff and a team's context
// reset of the same provider take the same path: the transcript is written, the old thread is
// kept for scoped deletion, and the next turn starts a fresh thread pointed at the transcript.
#include "runtime/ProviderHandoff.h"
#include "runtime/SessionHost.h"

#include <QJsonDocument>

namespace ketplus::daemon {

std::optional<SessionHost::FreshContextUndo> SessionHost::startFreshContext(Session& session,
                                                                            const QString& adapter,
                                                                            QString* transcript,
                                                                            QString* error) {
    const QString sessionId = session.stored.id;
    FreshContextUndo undo{.adapter = session.stored.adapter,
                          .threadId = session.stored.threadId,
                          .providerThreads = session.stored.providerThreads,
                          .attachedToProvider = session.attachedToProvider};
    QJsonObject threads = QJsonDocument::fromJson(session.stored.providerThreads.toUtf8()).object();
    provider_handoff::rememberThread(threads, undo.adapter, undo.threadId);
    *transcript = provider_handoff::writeTranscript(
        handoffDirectory(sessionId),
        provider_handoff::renderTranscript(
            provider_handoff::journalEvents(*journal_, sessionId, session.stored.generation)),
        error);
    if (transcript->isEmpty())
        return std::nullopt;
    if (!undo.threadId.isEmpty())
        sessionsByThread_.remove(undo.threadId);
    session.stored.adapter = adapter;
    session.stored.threadId.clear();
    session.stored.providerThreads =
        QString::fromUtf8(QJsonDocument(threads).toJson(QJsonDocument::Compact));
    session.attachedToProvider = false;
    return undo;
}

void SessionHost::undoFreshContext(Session& session, const FreshContextUndo& undo) {
    session.stored.adapter = undo.adapter;
    session.stored.threadId = undo.threadId;
    session.stored.providerThreads = undo.providerThreads;
    session.attachedToProvider = undo.attachedToProvider;
    if (!undo.threadId.isEmpty())
        sessionsByThread_.insert(undo.threadId, session.stored.id);
}

void SessionHost::resetTeamContext(Session& session) {
    const QString sessionId = session.stored.id;
    const QString hint = teamWakeTasks_.take(sessionId);
    if (!teams_)
        return;
    const auto plan = teams_->planContextReset(sessionId, hint, session.stored.threadId,
                                               session.stored.currentTurnSequence);
    if (!plan)
        return;
    QString transcript;
    QString failure;
    const auto undo = startFreshContext(session, session.stored.adapter, &transcript, &failure);
    if (!undo) {
        qWarning("Keeping the context of %s: %s", qUtf8Printable(sessionId),
                 qUtf8Printable(failure));
        return;
    }
    if (!appendEvent(session, QStringLiteral("session.context_reset"),
                     {{QStringLiteral("reason"), plan->reason},
                      {QStringLiteral("provider"), session.stored.adapter},
                      {QStringLiteral("task_id"), plan->task}},
                     &failure)) {
        undoFreshContext(session, *undo);
        qWarning("Keeping the context of %s: %s", qUtf8Printable(sessionId),
                 qUtf8Printable(failure));
        return;
    }
    session.contextResetNote = provider_handoff::contextResetNote(transcript) + plan->briefing;
    teams_->noteContextReset(sessionId, plan->task, session.stored.currentTurnSequence);
}

} // namespace ketplus::daemon
