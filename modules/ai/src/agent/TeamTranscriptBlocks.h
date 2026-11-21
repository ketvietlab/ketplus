#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>

namespace ketplus::team_blocks {

// #468: what a team sends into a member's conversation is a structured message of the daemon
// (ADR 0114): an assignment (#457), the leader's digest (#459), a review request or its
// verdict (ADR 0067) and an answered decision (#462). This reads one into the block the
// transcript draws:
//   kind      "assignment", "digest", "review", "verdict" or "decision"
//   team      the team's name
//   taskId    the task the block opens, or empty
//   title     the task's title, or what the block is about
//   facts     short facts (lane, branch, who assigned it, the round)
//   text      free text (a description, the findings, what the answer unblocks)
//   criteria  [{text, done, checked}]; `checked` is false where the message has no state
//   files     changed files of a review request
//   groups    the digest's [{taskId, title, state, lines: [{text, needsYou}]}]
//   changes   how many lines the digest has
//   question, answer, decisionId   an answered decision
// An empty map means the message is not one of these, or reads unlike them: the row stays
// the message's text, so a kind this client does not know loses nothing.
QVariantMap fromMessage(const QJsonObject& agentMessage);

// One block for a user.message that delivers exactly one team message; empty otherwise.
QVariantMap fromAgentMessages(const QJsonArray& agentMessages);

// The screen-reader name of a block: its kind and title, as one item.
QString accessibleName(const QVariantMap& block);

} // namespace ketplus::team_blocks
