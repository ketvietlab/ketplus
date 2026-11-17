// Internal to SidebarRepositoryList: what its source files share.
#pragma once

#include "KetConversationRow.h"

#include <QSet>
#include <QString>
#include <QVariantMap>

namespace ketplus::sidebar_list {

// Conversations a repository shows before "Show N more".
inline constexpr int kPreviewConversationCount = 5;
// Newest rows stay first. Running and selected conversations keep a slot even when that pushes
// the visible set past five. All of them with `showAll`.
[[nodiscard]] QSet<QString> previewedConversationIds(const QVariantList& conversations,
                                                     bool showAll);

// A conversation's state as the sidebar shows it.
[[nodiscard]] ket::ConversationStatus conversationStatus(const QVariantMap& conversation);
// The conversation whose message started a running turn; empty once the turn ends.
[[nodiscard]] QString runningAgentSender(const QVariantMap& conversation);
// Whether a conversation waits for the user: a question, a reply, an error, or a finished turn
// not opened yet.
[[nodiscard]] bool waitsForUser(const QVariantMap& conversation, bool seen);

} // namespace ketplus::sidebar_list
