#pragma once

#include <QJsonObject>
#include <QWidget>

namespace ketplus {

class MachineConversationSession;

// The approval and question cards of one remote conversation. Each answer goes through the
// conversation's own machine (ADR 0119 "Approvals and questions"); while the machine cannot act
// the buttons are disabled, and a card whose answer failed shows as expired.
class MachineApprovalCards final : public QWidget {
    Q_OBJECT

  public:
    explicit MachineApprovalCards(QWidget* parent = nullptr);

    void setSession(MachineConversationSession* session);
    void refresh();

  private:
    QWidget* createCard(const QJsonObject& request, bool canAct);
    QWidget* createQuestionCard(const QJsonObject& request, bool canAct);

    MachineConversationSession* session_{nullptr};
};

} // namespace ketplus
