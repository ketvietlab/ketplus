#pragma once

#include "teams/TeamAgentDialog.h"

#include <QJsonObject>
#include <QVariantList>
#include <QWidget>

#include <functional>

class QLabel;

namespace ket {
class Button;
class Dialog;
class FieldRow;
class TextField;
} // namespace ket

namespace ketplus {

class TeamDirectory;

// T-23: the team's second reviewer, a new Claude conversation. Two reviewers review two
// tasks at the same time; a team has at most two (ADR 0114).
class AddReviewerForm final : public QWidget {
    Q_OBJECT

  public:
    AddReviewerForm(TeamDirectory* teams, const QJsonObject& team, const QVariantList& models,
                    QWidget* parent = nullptr);

    [[nodiscard]] QJsonObject payload() const;
    bool validate();
    void submit(std::function<void()> added);
    // The team has two reviewers already.
    [[nodiscard]] bool isFull() const;
    [[nodiscard]] QString error() const;
    [[nodiscard]] ket::TextField* nameField() const { return name_; }
    void setAgent(const TeamAgent& agent);

    static ket::Dialog* present(QWidget* anchor, TeamDirectory* teams, const QJsonObject& team,
                                const QVariantList& models);

  private:
    void showError(const QString& text);

    TeamDirectory* teams_;
    QJsonObject team_;
    QVariantList models_;
    TeamAgent agent_;
    ket::TextField* name_;
    ket::Button* agentButton_;
    ket::FieldRow* nameRow_;
    ket::FieldRow* agentRow_;
    QLabel* error_;
};

} // namespace ketplus
