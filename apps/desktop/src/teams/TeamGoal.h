#pragma once

#include "KetTextArea.h"

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantList>

#include <functional>

class QWidget;

namespace ket {
class ExpandableText;
} // namespace ket

namespace ketplus {

// ADR 0114: a team's goal. It is the leader's main input, so it is a multi-line kit field
// (ket::TextArea) with `@agent/<number>` mentions of the conversations on this computer; the
// leader may ask them for context.
namespace team_goal {

// The daemon's limit (teams::rules::maximumGoalCharacters); above it the form refuses.
inline constexpr int maximumCharacters = 16000;
inline constexpr int visibleLines = 6;
inline constexpr int growLines = 14;
// The suggestions the picker shows, as the composer's `@agent/` completion does.
inline constexpr int maximumSuggestions = 8;

// Numbered conversations on this computer ({number, title, provider, running, path}), as
// AgentSessionManager::numberedConversations() lists them.
using ConversationDirectory = std::function<QVariantList()>;

[[nodiscard]] QString placeholder();
// The lines of a goal the team page shows before Show more.
inline constexpr int foldedLines = 3;
// The team page's goal: folded after foldedLines, line breaks kept, its mentions links named
// after their conversations, and numbers `directory` does not hold marked.
[[nodiscard]] ket::ExpandableText* foldedGoal(const QString& goal, const QVariantList& directory,
                                              QWidget* parent);
// The numbers `text` mentions that `directory` does not hold, each once, in order.
[[nodiscard]] QStringList unknownMentions(const QString& text, const QVariantList& directory);
// What the form says about them, or about a goal above the limit; empty when it can be sent.
[[nodiscard]] QString problem(const QString& text, const QStringList& unknown);

} // namespace team_goal

// Sets a ket::TextArea up as a team's goal: its lines, limit and placeholder, `@` completion
// from the conversation directory, and unknown numbers marked as the user types. Typing a
// plain `@` where a mention can start completes it to `@agent/`, since the goal mentions only
// conversations.
class TeamGoalEditor final : public QObject {
    Q_OBJECT

  public:
    TeamGoalEditor(ket::TextArea* field, team_goal::ConversationDirectory directory);

    [[nodiscard]] ket::TextArea* field() const { return field_; }
    [[nodiscard]] QStringList unknownMentions() const { return unknown_; }
    // The form's message for the goal as it is now; empty when it can be sent.
    [[nodiscard]] QString problem() const;

  signals:
    void problemChanged(const QString& problem);

  private:
    void suggest(const QString& query);
    void refresh();
    void completeMention(int position);

    QPointer<ket::TextArea> field_;
    team_goal::ConversationDirectory directory_;
    QStringList unknown_;
    QString problem_;
    int pendingAt_ = -1;
};

} // namespace ketplus
