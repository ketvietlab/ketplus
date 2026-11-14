#pragma once

// ADR 0067 and ADR 0114 §2 (ketplus#295): which conversations may review. Claude enforces the
// read-only review guard on every tool call. Codex reviews only under the conversation's own
// Full access, and the daemon checks the reviewed branch after each of its review turns
// (ReviewIntegrity). Cursor forwards prompts and never reviews.
#include <QString>

namespace ketplus::daemon::reviewer {

// A Codex reviewer's turns run with Full access; the branch check stands in for the guard.
[[nodiscard]] inline bool checksBranch(const QString& provider) {
    return provider == QStringLiteral("codex");
}

[[nodiscard]] inline bool supported(const QString& provider, const QString& executionPolicy) {
    if (provider == QStringLiteral("claude"))
        return true;
    return checksBranch(provider) && executionPolicy == QStringLiteral("full");
}

// Why a conversation cannot review, in the words the clients show.
[[nodiscard]] inline QString unsupportedMessage(const QString& provider) {
    if (checksBranch(provider))
        return QStringLiteral("Codex can review only with Full access.");
    return QStringLiteral("A reviewer must be a Claude conversation, or Codex with Full access.");
}

} // namespace ketplus::daemon::reviewer
