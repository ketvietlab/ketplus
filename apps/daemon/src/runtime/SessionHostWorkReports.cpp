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

// ADR 0106: the stored ledger is authoritative; the event carries the snapshot for the
// transcript, so a turn shows the report it ended with even after later turns change it.
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
    QJsonObject answer = work_report::summary(merged);
    // A retained event reaches the transcript when the journal recovers; the ledger is stored.
    if (!appendEvent(*it, QStringLiteral("work_report.updated"), merged.ledger))
        answer.insert(QStringLiteral("note"),
                      QStringLiteral("The report is saved; it appears in the transcript once "
                                     "the journal recovers."));
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
            {QStringLiteral("report"),
             journal_->workReport(session->stored.id, session->stored.generation)}};
}

} // namespace ketplus::daemon
