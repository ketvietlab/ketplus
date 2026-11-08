#include "storage/LocalJournal.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>

namespace ketplus::daemon {

bool LocalJournal::setConversationReviewer(const ConversationReviewer& reviewer, QString* error) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "INSERT INTO conversation_reviewers (session_id, reviewer_session_id, auto_fix, "
        "max_rounds, updated_at) VALUES (?, ?, ?, ?, ?) ON CONFLICT(session_id) DO UPDATE SET "
        "reviewer_session_id = excluded.reviewer_session_id, auto_fix = excluded.auto_fix, "
        "max_rounds = excluded.max_rounds, updated_at = excluded.updated_at"));
    query.addBindValue(reviewer.sessionId);
    query.addBindValue(reviewer.reviewerSessionId);
    query.addBindValue(reviewer.autoFix ? 1 : 0);
    query.addBindValue(reviewer.maxRounds);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (query.exec())
        return true;
    if (error)
        *error = query.lastError().text();
    return false;
}

bool LocalJournal::clearConversationReviewer(const QString& sessionId, QString* error) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("DELETE FROM conversation_reviewers WHERE session_id = ?"));
    query.addBindValue(sessionId);
    if (query.exec())
        return true;
    if (error)
        *error = query.lastError().text();
    return false;
}

std::optional<ConversationReviewer>
LocalJournal::conversationReviewer(const QString& sessionId) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT reviewer_session_id, auto_fix, max_rounds, updated_at "
                                 "FROM conversation_reviewers WHERE session_id = ?"));
    query.addBindValue(sessionId);
    if (!query.exec() || !query.next())
        return std::nullopt;
    return ConversationReviewer{.sessionId = sessionId,
                                .reviewerSessionId = query.value(0).toString(),
                                .autoFix = query.value(1).toInt() != 0,
                                .maxRounds = query.value(2).toInt(),
                                .updatedAt = query.value(3).toString()};
}

int LocalJournal::reviewRequestsInChain(const QString& chainId, const QString& sessionId) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM agent_messages WHERE chain_id = ? AND "
                                 "from_session = ? AND kind = 'review'"));
    query.addBindValue(chainId);
    query.addBindValue(sessionId);
    return query.exec() && query.next() ? query.value(0).toInt() : 0;
}

} // namespace ketplus::daemon
