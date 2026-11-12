#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

namespace ketplus {

class MachineApprovalCards;
class MachineCatalog;
class MachineConversationActions;
class MachineConversationSession;
class MachineRichTranscript;

// ADR 0119: the Agent Workspace for a machine other than This Mac. It reads the selected
// machine through MachineCatalog and a MachineConversationSession, never a transport: the
// transcript, composer, approval and question cards, and the worktree picker of a new
// conversation (MachineBackend::createSession) are the same on the SSH and Relay routes.
// Each machine keeps the conversation it last showed, so switching back restores it.
class MachineWorkspaceView final : public QWidget {
    Q_OBJECT

  public:
    explicit MachineWorkspaceView(MachineCatalog* catalog, QWidget* parent = nullptr);
    ~MachineWorkspaceView() override;

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
    // The transcript renderer (This Mac's, in transcript-only mode), for its theme and tests.
    [[nodiscard]] MachineRichTranscript* transcript() const { return transcript_; }

  signals:
    // A conversation was opened or created, for the sidebar's selection.
    void conversationOpened(const QString& machineId, const QString& sessionId);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void setupToastAvoidance();
    void refreshToastAncestors();
    void updateToastAvoidance();
    void clearToastAvoidance();
    QList<QPointer<QWidget>> toastAncestors_;
    QPointer<QWidget> toastHost_;
    bool toastTeardown_{false};

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
    struct ForkOrigin {
        QString machineId;
        QString openSessionId; // what the reader read when they asked; empty for the composer
    };
    QHash<QString, ForkOrigin> forkOrigins_; // fork request id -> where it was asked
    QLabel* banner_{nullptr};
    QLabel* title_{nullptr};
    QToolButton* more_{nullptr};
    QComboBox* worktrees_{nullptr};
    MachineRichTranscript* transcript_{nullptr};
    MachineApprovalCards* approvals_{nullptr};
    QPlainTextEdit* composer_{nullptr};
    QLabel* composerReason_{nullptr};
    QPushButton* attach_{nullptr};
    QPushButton* send_{nullptr};
    QPushButton* stop_{nullptr};
};

} // namespace ketplus
