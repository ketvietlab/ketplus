#include "runtime/SessionHost.h"

#include "agents/WorkReport.h"

namespace ketplus::daemon {
namespace {

QJsonObject failure(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

QJsonObject publishedReport(LocalJournal& journal, const QString& id, const qint64 generation) {
    auto report = journal.publishedWorkReport(id, generation);
    const auto pending = journal.workReport(id, generation);
    // Only request/version metadata comes from the mutable ledger. In particular, a pending
    // completion must not remove an unresolved item from the last published snapshot.
    if (report.value(QStringLiteral("request_id")) != pending.value(QStringLiteral("request_id")))
        report.remove(QStringLiteral("title"));
    if (pending.contains(QStringLiteral("request_id")))
        report.insert(QStringLiteral("request_id"), pending.value(QStringLiteral("request_id")));
    report.insert(QStringLiteral("version"), pending.value(QStringLiteral("version")));
    return work_report::currentRequest(report);
}

} // namespace

// Called only after accepted input events have committed and been delivered. Use the
// ordinary durable report event so an already-open Report pane refreshes without a tool call.
void SessionHost::publishWorkReportScope(Session& session, const qint64 acceptedCursor) {
    const auto ledger = journal_->workReport(session.stored.id, session.stored.generation);
    if (!ledger.contains(QStringLiteral("items")) ||
        ledger.value(QStringLiteral("request_id")).toInteger(-1) != acceptedCursor)
        return;
    auto report = publishedReport(*journal_, session.stored.id, session.stored.generation);
    report.insert(QStringLiteral("scope_only"), true);
    (void)appendEvent(session, QStringLiteral("work_report.updated"), report);
}

// ADR 0106: the stored ledger is authoritative. Tool calls remain pending until the
// actual terminal event captures this turn's latest scoped snapshot.
QJsonObject SessionHost::recordWorkReport(const QString& sessionId, const qint64 generation,
                                          const QJsonObject& payload) {
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end() || it->stored.generation != generation)
        return failure(QStringLiteral("session_not_found"),
                       QStringLiteral("This conversation no longer exists."));
    const QJsonObject current = journal_->workReport(sessionId, generation);
    const work_report::Merge merged =
        work_report::merge(current, payload, it->stored.currentTurnSequence);
    if (!merged.error.isEmpty())
        return failure(QStringLiteral("invalid_report"), merged.error);
    if (QString error; !journal_->storeWorkReport(sessionId, generation, merged.ledger, &error))
        return failure(QStringLiteral("journal_error"), error);
    // Tool success acknowledges durable pending data, not the end of a turn. Only
    // turnCompletion publishes the final snapshot; further tools/steers may still follow.
    QJsonObject answer = work_report::summary(merged);
    answer.insert(QStringLiteral("ok"), true);
    return answer;
}

// The Report tab reads terminal snapshots only, scoped to the current request. Opening or
// reopening it during a turn must never expose pending tool updates.
QJsonObject SessionHost::readWorkReport(const QJsonObject& request) {
    Session* session = nullptr;
    if (const auto checked = checkedSession(request, &session); !checked.isEmpty())
        return checked;
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("report"),
             publishedReport(*journal_, session->stored.id, session->stored.generation)}};
}

} // namespace ketplus::daemon
