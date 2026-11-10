#pragma once

#include "git/GitTypes.h"

#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QAbstractButton;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QRadioButton;

namespace ket {
class Button;
class Dialog;
} // namespace ket

namespace ketplus {

// Whether a card named `cardName` is open over `window`. A closed card lingers until the
// event loop deletes it, so finding one is not enough.
[[nodiscard]] bool agentDialogOpen(const QWidget* window, const QString& cardName);

// The form of the "Create worktree" and "New conversation" dialogs. present() shows it as an
// in-window ket::Dialog card, as the mock's KetDialog (#422).
class AgentWorktreeDialog final : public QWidget {
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
    // Create worktree only: whether Agent Workspace opens on the new worktree.
    [[nodiscard]] bool startConversation() const;
    // Conversation layout only: replaces the listed worktrees, keeping the one the
    // reader picked when it is still there.
    void setWorktrees(const QVector<GitWorktree>& worktrees);
    // Opens the form in a card over `anchor`'s window and returns the card, which owns the
    // form and deletes itself once closed. The card accepts only a valid form: read the
    // answers in a slot on its accepted().
    ket::Dialog* present(QWidget* anchor);

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
    QAbstractButton* startConversation_{nullptr};
    QLabel* error_{nullptr};
    QLabel* status_{nullptr};
    // The card's primary action, once presented.
    QPointer<ket::Button> confirm_;
    QPointer<ket::Dialog> card_;
    QSet<QString> localBranches_;
    quint64 branchLoadGeneration_{0};
    bool branchesReady_{false};
};

// The form of "Delete conversation"; present() shows it as an in-window ket::Dialog card.
class AgentConversationDeleteDialog final : public QWidget {
    Q_OBJECT

  public:
    // `linkedWorktreePaths` are exactly the checkouts deletion would remove; they are listed so
    // the user sees what goes before opting in.
    explicit AgentConversationDeleteDialog(const QStringList& linkedWorktreePaths,
                                           QWidget* parent = nullptr);
    [[nodiscard]] bool deleteLinkedWorktree() const;
    [[nodiscard]] bool forceDeleteWorktree() const;
    // As AgentWorktreeDialog::present.
    ket::Dialog* present(QWidget* anchor);

  private:
    void updateConfirm();

    QCheckBox* deleteLinked_{nullptr};
    QCheckBox* force_{nullptr};
    QPointer<ket::Button> confirm_;
};

} // namespace ketplus
