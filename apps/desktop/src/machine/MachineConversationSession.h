#pragma once

#include "MachineBackend.h"
#include "machine/MachineTranscript.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QVector>

#include <memory>

namespace ketplus {

// ADR 0119 "Agent Workspace": one conversation opened on one machine. It keeps the backend it
// was opened with, so its transcript paging, turns, steer, stop and approval answers go to that
// machine even after the picker selects another. The transcript is cached here and stays
// readable while the machine is Reconnecting or Offline; the composer and approval buttons are
// then disabled with the reason, and a return to Connected reloads the window and the pending
// approvals on its own.
class MachineConversationSession final : public QObject {
    Q_OBJECT

  public:
    using State = remote::MachineBackend::ConnectionState;

    MachineConversationSession(remote::MachineBackend* backend, QString sessionId,
                               qint64 generation, QObject* parent = nullptr);

    [[nodiscard]] QString machineId() const { return machineId_; }
    [[nodiscard]] QString sessionId() const { return sessionId_; }
    [[nodiscard]] qint64 generation() const { return generation_; }
    [[nodiscard]] const MachineTranscript& transcript() const { return transcript_; }
    // Pending approval and question requests as the machine sent them, each with
    // `approval_id` and `kind` ("input" for a question). A request whose answer failed carries
    // `expired: true` and stays until the next reload.
    [[nodiscard]] const QVector<QJsonObject>& approvals() const { return approvals_; }
    [[nodiscard]] bool running() const { return running_; }
    void setRunning(bool running);

    // Whether the composer and approval buttons may act, and if not, why.
    [[nodiscard]] bool canAct() const;
    [[nodiscard]] QString blockedReason() const;
    [[nodiscard]] QString lastError() const { return lastError_; }
    // What happened to the last writes: held until the machine reconnects, sending, or queued
    // on the Relay until the host confirms. Empty when nothing is outstanding.
    [[nodiscard]] QString writeStatus() const;

    ~MachineConversationSession() override;

    // Reads the newest transcript window and the pending approvals; the route follows the
    // conversation's events from then on.
    void load();
    // The conversation is no longer shown: the route stops following it
    // (MachineBackend::conversationClosed). The cached transcript stays.
    void close();
    // A prompt: a new turn, or a steer of the running one.
    void send(const QString& text);
    void stop();
    void resolveApproval(const QString& approvalId, bool accept);
    void answerQuestion(const QString& questionId, const QJsonObject& resolution);

  signals:
    void changed();

  private:
    void loadApprovals();
    void loadApprovalsPage(const QString& afterId,
                           const std::shared_ptr<QVector<QJsonObject>>& collected, quint64 load);
    void applyEvent(const QJsonObject& event);
    void markExpired(const QString& approvalId, const QString& error);
    // The callback of one write; `approvalId` names the card it answers, if any.
    [[nodiscard]] remote::MachineBackend::Callback write(const QString& approvalId);
    void commandCompleted(const QString& commandId, const QJsonObject& result,
                          const QString& error);
    void finishWrite(const QString& approvalId, const QString& error);
    void fail(const QString& error);
    [[nodiscard]] remote::MachineBackend::Callback
    guarded(std::function<void(const QJsonObject&, const QString&)> done);

    QPointer<remote::MachineBackend> backend_;
    QString machineId_;
    QString sessionId_;
    qint64 generation_{0};
    MachineTranscript transcript_;
    QVector<QJsonObject> approvals_;
    QString lastError_;
    quint64 approvalsLoad_{0};
    int waiting_{0};
    bool open_{false};
    QHash<QString, QString> queued_; // Relay command id -> the card it answers (or empty)
    bool running_{false};
    State lastState_{State::Connecting};
};

} // namespace ketplus
