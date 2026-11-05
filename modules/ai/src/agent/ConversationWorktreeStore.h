#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ketplus {

struct ConversationWorktreeRecord final {
    QString conversationId;
    QString path;
    QString role;
    bool hidden{false};
};

// Desktop-owned SQLite map from conversation id to Git worktrees that conversation
// created. Written at create time and at turn completion; never reconstructed from
// the daemon journal.
class ConversationWorktreeStore final {
  public:
    static constexpr auto kRolePrimary = "primary";
    static constexpr auto kRoleAgent = "agent";

    explicit ConversationWorktreeStore(QString databasePath = {});

    void save(const QString& conversationId, const QString& path, const QString& role);
    void hide(const QString& conversationId, const QString& path);
    void removeConversation(const QString& conversationId);
    [[nodiscard]] QVector<ConversationWorktreeRecord> list(const QString& conversationId) const;
    [[nodiscard]] QStringList visiblePaths(const QString& conversationId) const;
    [[nodiscard]] QStringList allPaths(const QString& conversationId) const;
    [[nodiscard]] bool containsPath(const QString& path,
                                    const QString& exceptConversationId = {}) const;

    static QStringList createdDuringTurn(const QStringList& before, const QStringList& after,
                                         const QString& mainCheckout);

  private:
    // list() runs whenever the conversation list changes; rows change only through this
    // object, so reads are cached per conversation and every write drops that entry.
    mutable QHash<QString, QVector<ConversationWorktreeRecord>> cache_;
    QString databasePath_;
};

} // namespace ketplus
