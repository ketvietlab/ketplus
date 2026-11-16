#include "runtime/SessionHost.h"

#include "agents/WorkReport.h"

namespace ketplus::daemon {
namespace {

QJsonObject failure(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

} // namespace

// Called only after accepted input events have committed and been delivered. Use the
// ordinary durable report event so an already-open Report pane refreshes without a tool call.
void SessionHost::publishWorkReportScope(Session& session, const qint64 acceptedCursor) {
    const auto ledger = journal_->workReport(session.stored.id, session.stored.generation);
    if (!ledger.contains(QStringLiteral("items")) ||
        ledger.value(QStringLiteral("request_id")).toInteger(-1) != acceptedCursor)
        return;
    auto report = work_report::currentRequest(ledger);
    report.insert(QStringLiteral("turn"), session.stored.currentTurnSequence);
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

// session.work_report.read, for the desktop's Report tab: the current ledger, empty before the
// conversation's first report.
QJsonObject SessionHost::readWorkReport(const QJsonObject& request) {
    Session* session = nullptr;
    if (const auto checked = checkedSession(request, &session); !checked.isEmpty())
        return checked;
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("report"), work_report::currentRequest(journal_->workReport(
                                           session->stored.id, session->stored.generation))}};
}

} // namespace ketplus::daemon
