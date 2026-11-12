#pragma once

#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QWidget>

class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

namespace ketplus {

class MachineApprovalCards;
class MachineCatalog;
class MachineConversationActions;
class MachineConversationSession;

// ADR 0119: the Agent Workspace for a machine other than This Mac. It reads the selected
// machine through MachineCatalog and a MachineConversationSession, never a transport: the
// transcript, composer, approval and question cards, and the worktree picker of a new
// conversation (MachineBackend::createSession) are the same on the SSH and Relay routes.
// Each machine keeps the conversation it last showed, so switching back restores it.
class MachineWorkspaceView final : public QWidget {
    Q_OBJECT

  public:
    explicit MachineWorkspaceView(MachineCatalog* catalog, QWidget* parent = nullptr);

    // Opens a conversation of the selected machine; it stays bound to that machine.
    void openConversation(const QString& sessionId);
    // Shows the new-conversation composer with its worktree picker.
    void startNewConversation(const QString& repositoryId = {});
    [[nodiscard]] MachineConversationSession* session() const;
    [[nodiscard]] QString openSessionId() const;
    // Rename, fork and archive of this machine's conversations (the sidebar menu uses it too).
    [[nodiscard]] MachineConversationActions* actions() const { return actions_; }
    // A fork the machine confirmed, opened once it is listed; empty when none waits.
    [[nodiscard]] QString pendingFork() const { return pendingFork_; }

  signals:
    // A conversation was opened or created, for the sidebar's selection.
    void conversationOpened(const QString& machineId, const QString& sessionId);

  private:
    void catalogChanged();
    void render();
    void renderWorktrees();
    void renderTranscript();
    void renderActions(bool newConversation);
    void submit();
    void createConversation(const QString& prompt);
    // `reload` reads the conversation again (and the route follows it) when it was not shown.
    void showSession(MachineConversationSession* session, bool reload);
    void bind(const QString& sessionId, qint64 generation, bool running);
    void finishCreate(const QString& machine, const QJsonObject& result, const QString& error);

    MachineCatalog* catalog_;
    QPointer<MachineConversationSession> session_;
    // machine id -> its conversation; a machine's conversations are dropped with its backend.
    QHash<QString, QPointer<MachineConversationSession>> lastByMachine_;
    QString newConversationRepository_;
    bool creating_{false};
    QString createCommand_; // a createSession the Relay queued
    QString createError_;
    QMetaObject::Connection createCompletion_;
    MachineConversationActions* actions_{nullptr};
    QString actionError_;
    QString pendingFork_; // opened once the machine lists it (the Relay verifies it first)
    QString pendingForkMachine_;
    QLabel* banner_{nullptr};
    QLabel* title_{nullptr};
    QToolButton* more_{nullptr};
    QLabel* transcriptNote_{nullptr};
    QPushButton* older_{nullptr};
    QString renderedSession_;
    qint64 renderedFirstCursor_{0};
    int renderedRows_{0};
    QComboBox* worktrees_{nullptr};
    QListWidget* transcript_{nullptr};
    MachineApprovalCards* approvals_{nullptr};
    QPlainTextEdit* composer_{nullptr};
    QLabel* composerReason_{nullptr};
    QPushButton* attach_{nullptr};
    QPushButton* send_{nullptr};
    QPushButton* stop_{nullptr};
};

} // namespace ketplus
