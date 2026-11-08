#pragma once

#include <QString>

namespace ketplus::daemon {

// The conversation that reviews another conversation's finished turns (ADR 0067).
struct ConversationReviewer final {
    QString sessionId;
    QString reviewerSessionId;
    // Whether a review with findings starts a turn of the working conversation.
    bool autoFix{true};
    // Reviews per chain of turns that a user turn started.
    int maxRounds{2};
    QString updatedAt;
};

} // namespace ketplus::daemon
