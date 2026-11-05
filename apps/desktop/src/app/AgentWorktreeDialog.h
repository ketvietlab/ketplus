#pragma once

#include "git/GitTypes.h"

#include <QDialog>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QRadioButton;

namespace ketplus {

class AgentWorktreeDialog final : public QDialog {
    Q_OBJECT

  public:
    enum class Purpose { Worktree, Conversation };

    explicit AgentWorktreeDialog(const QString& repositoryPath, QWidget* parent = nullptr,
                                 Purpose purpose = Purpose::Worktree,
                                 QString primaryWorktreePath = {},
                                 QVector<GitWorktree> worktrees = {});
    [[nodiscard]] QString branchName() const;
    [[nodiscard]] QString baseBranch() const;
    [[nodiscard]] QString selectedWorktreePath() const;
    [[nodiscard]] bool createWorktree() const;
    // Conversation layout only: replaces the listed worktrees, keeping the one the
    // reader picked when it is still there.
    void setWorktrees(const QVector<GitWorktree>& worktrees);

  private:
    void populateWorktrees(const QVector<GitWorktree>& worktrees, const QString& repositoryPath);
    void loadLocalBranches(const QString& root);
    void updateActions();
    void tryAccept();
    // The combo a branch list is published to: the base-branch field of whichever
    // layout is in use.
    [[nodiscard]] QComboBox* branchCombo() const;
    // Enables only the chosen section, so the dialog reads as one decision.
    void updateSections();
    [[nodiscard]] bool branchesReady() const;
    // Contains-matching, case-insensitive completion for a long list.
    static void makeSearchable(QComboBox* combo);
    [[nodiscard]] bool creatingWorktree() const;

    Purpose purpose_{Purpose::Worktree};
    QString primaryWorktreePath_;
    // The worktree the dialog was opened from, selected when nothing else was picked.
    QString anchorPath_;
    QLineEdit* branch_{nullptr};
    QComboBox* base_{nullptr};
    // Conversation layout only: the two sections and the base branch a new
    // worktree starts from.
    QComboBox* newWorktreeBase_{nullptr};
    QRadioButton* existingWorktree_{nullptr};
    QRadioButton* newWorktree_{nullptr};
    QLabel* error_{nullptr};
    QLabel* status_{nullptr};
    QSet<QString> localBranches_;
    quint64 branchLoadGeneration_{0};
    bool branchesReady_{false};
};

class AgentConversationDeleteDialog final : public QDialog {
    Q_OBJECT

  public:
    // `linkedWorktreePaths` are exactly the checkouts deletion would remove; they are listed so
    // the user sees what goes before opting in.
    explicit AgentConversationDeleteDialog(const QStringList& linkedWorktreePaths,
                                           QWidget* parent = nullptr);
    [[nodiscard]] bool deleteLinkedWorktree() const;
    [[nodiscard]] bool forceDeleteWorktree() const;

  private:
    QCheckBox* deleteLinked_{nullptr};
    QCheckBox* force_{nullptr};
};

} // namespace ketplus
