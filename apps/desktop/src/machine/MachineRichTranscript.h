#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>

class QWidget;

namespace ketplus {

class AgentWorkspaceView;
class MachineConversationSession;

// ketplus#598: the transcript of a conversation on another machine, drawn by This Mac's rich
// renderer (AgentWorkspaceView in transcript-only mode). The rows are projected from the
// session's cached journal events, in the shape remote::windowEvent() delivers, by the same
// agent_message::daemonWindowMessages() This Mac uses, so Markdown, tool calls, diffs and
// activity groups read the same on SSH and on the Relay. An older page is prepended, a live
// event appends or updates its row, and the renderer's own paging asks the session for the page
// before the oldest cached event.
class MachineRichTranscript final : public QObject {
    Q_OBJECT

  public:
    explicit MachineRichTranscript(QWidget* parent);

    [[nodiscard]] AgentWorkspaceView* view() const { return view_; }
    // Shows `session` (or nothing), bringing the renderer up to date with its cached events.
    void show(MachineConversationSession* session);
    // The rows last given to the renderer, oldest first.
    [[nodiscard]] const QVariantList& rows() const { return rows_; }

  signals:
    // Why an action of the transcript is unavailable (it needs this Mac's files or panes).
    void actionRefused(const QString& reason);

  private:
    void sync(const QString& scope, const QVariantList& rows);
    // Updates the rows the renderer shows under the same key with other contents.
    void updateChanged(const QVariantList& rows);
    void publishHistory();

    AgentWorkspaceView* view_{nullptr};
    QPointer<MachineConversationSession> session_;
    QString scope_;
    QVariantList rows_;
    qint64 requestToken_{0};
};

} // namespace ketplus
