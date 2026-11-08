#include "runtime/SessionHost.h"

#include <QSet>

namespace ketplus::daemon {
namespace {
// ADR 0027 holds messages past hop 6; three rounds of review and reply stay within it.
constexpr int maximumReviewRounds = 3;

QJsonObject failure(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

bool enforcesReviewGuard(const QString& adapter) {
    return adapter == QStringLiteral("claude") || adapter == QStringLiteral("codex");
}
} // namespace

QJsonObject SessionHost::reviewerJson(const QString& sessionId) const {
    const auto reviewer = journal_->conversationReviewer(sessionId);
    if (!reviewer)
        return {};
    QJsonObject json{{QStringLiteral("session_id"), reviewer->reviewerSessionId},
                     {QStringLiteral("auto_fix"), reviewer->autoFix},
                     {QStringLiteral("max_rounds"), reviewer->maxRounds}};
    if (const auto it = sessions_.constFind(reviewer->reviewerSessionId); it != sessions_.cend()) {
        json.insert(QStringLiteral("number"),
                    journal_->conversationNumber(it->stored.id, it->stored.generation));
        json.insert(QStringLiteral("title"), it->stored.title);
        json.insert(QStringLiteral("provider"), it->stored.adapter);
        json.insert(QStringLiteral("model"), it->stored.model);
    }
    return json;
}

// ADR 0067: session.reviewer.get and session.reviewer.set, for authenticated local clients.
QJsonObject SessionHost::conversationReviewer(const QJsonObject& request, const bool apply) {
    Session* session = nullptr;
    if (const auto checked = checkedSession(request, &session); !checked.isEmpty())
        return checked;
    const QString sessionId = session->stored.id;
    if (apply) {
        const auto payload = request.value(QStringLiteral("payload")).toObject();
        QString reviewerId = payload.value(QStringLiteral("reviewer_session_id")).toString();
        const QString newProvider = payload.value(QStringLiteral("new_provider")).toString();
        const int rounds = payload.value(QStringLiteral("max_rounds")).toInt(2);
        QString error;
        if (reviewerId.isEmpty() && newProvider.isEmpty()) {
            if (!journal_->clearConversationReviewer(sessionId, &error))
                return failure(QStringLiteral("journal_error"), error);
        } else {
            if (!reviewerId.isEmpty() && !newProvider.isEmpty())
                return failure(QStringLiteral("invalid_reviewer"),
                               QStringLiteral("Choose an existing reviewer or a new one."));
            if (reviewerId == sessionId)
                return failure(QStringLiteral("invalid_reviewer"),
                               QStringLiteral("A conversation cannot review itself."));
            if (!reviewerId.isEmpty() && !sessions_.contains(reviewerId))
                return failure(QStringLiteral("reviewer_not_found"),
                               QStringLiteral("The reviewer conversation no longer exists."));
            // Only these providers enforce the read-only review guard.
            if (!enforcesReviewGuard(reviewerId.isEmpty()
                                         ? newProvider
                                         : sessions_.value(reviewerId).stored.adapter))
                return failure(
                    QStringLiteral("invalid_reviewer"),
                    QStringLiteral("A reviewer must be a Claude or Codex conversation."));
            if (rounds < 1 || rounds > maximumReviewRounds)
                return failure(QStringLiteral("invalid_reviewer"),
                               QStringLiteral("Review rounds must be between 1 and 3."));
            if (reviewerId.isEmpty()) {
                reviewerId = startReviewerConversation(*session, newProvider, &error);
                if (reviewerId.isEmpty())
                    return failure(QStringLiteral("reviewer_unavailable"), error);
            }
            if (!journal_->setConversationReviewer(
                    {.sessionId = sessionId,
                     .reviewerSessionId = reviewerId,
                     .autoFix = payload.value(QStringLiteral("auto_fix")).toBool(true),
                     .maxRounds = rounds},
                    &error))
                return failure(QStringLiteral("journal_error"), error);
            agents_->noteReviewerAssigned(sessionId);
        }
    }
    const QJsonObject reviewer = reviewerJson(sessionId);
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("reviewer"), reviewer.isEmpty() ? QJsonValue() : QJsonValue(reviewer)}};
}

} // namespace ketplus::daemon
