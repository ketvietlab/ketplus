#pragma once

#include "teams/TeamAgentDialog.h"
#include "teams/TeamGoal.h"

#include <QHash>
#include <QJsonObject>
#include <QVariantList>
#include <QWidget>

class QBoxLayout;
class QLabel;

namespace ket {
class Button;
class Checkbox;
class Dialog;
class FieldRow;
class Radio;
class SegmentedControl;
class Select;
class TextArea;
class TextField;
} // namespace ket

namespace ketplus {

class TeamDirectory;

// #465: a whole team in one dialog. One team.create; on a refusal the dialog stays open with
// what was typed and the error at the role it names.
class NewTeamForm final : public QWidget {
    Q_OBJECT

  public:
    NewTeamForm(TeamDirectory* teams, const QString& repository, const QVariantList& models,
                team_goal::ConversationDirectory conversations = {}, QWidget* parent = nullptr);

    [[nodiscard]] QJsonObject payload() const;
    [[nodiscard]] int memberCount() const;
    // Marks what is missing; true when the request can be sent.
    bool validate();
    // Sends team.create; `created` runs with the new team on success.
    void submit(std::function<void(const QJsonObject& team)> created);

    [[nodiscard]] ket::TextField* nameField() const { return name_; }
    [[nodiscard]] ket::TextArea* goalField() const { return goal_; }
    [[nodiscard]] ket::TextField* budgetField() const { return budget_; }
    [[nodiscard]] ket::SegmentedControl* developerCount() const { return developers_; }
    [[nodiscard]] TeamAgent agent(const QString& role) const { return agents_.value(role); }
    void setAgent(const QString& role, const TeamAgent& agent);
    [[nodiscard]] ket::Button* agentButton(const QString& role) const {
        return agentButtons_.value(role);
    }
    [[nodiscard]] QString error(const QString& role) const { return errors_.value(role); }
    [[nodiscard]] QString hint() const;

    static ket::Dialog* present(QWidget* anchor, TeamDirectory* teams, const QString& repository,
                                const QVariantList& models,
                                std::function<void(const QJsonObject& team)> created = {},
                                team_goal::ConversationDirectory conversations = {});

  signals:
    void memberCountChanged(int count);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void chooseAgent(const QString& role);
    void showErrors(const QJsonObject& refusal);
    void clearErrors();
    void describePermissions();
    // The form follows the width of `viewport`, the scroll area it is shown in.
    void follow(QWidget* viewport);
    void fit();
    void setCompact(bool compact);

    TeamDirectory* teams_;
    QString repository_;
    QVariantList models_;
    ket::TextField* name_;
    ket::TextArea* goal_;
    TeamGoalEditor* goalEditor_;
    ket::TextField* branch_;
    ket::Radio* leaderMerges_;
    ket::Radio* userMerges_;
    ket::Select* permissions_;
    ket::FieldRow* permissionsRow_{nullptr};
    // The team's own automatic turns, filled with Settings → Teams (ADR 0114, open point 2).
    ket::TextField* budget_{nullptr};
    ket::SegmentedControl* developers_;
    ket::Checkbox* requirements_;
    QLabel* hint_;
    QLabel* error_;
    QHash<QString, TeamAgent> agents_;
    QHash<QString, ket::Button*> agentButtons_;
    QHash<QString, ket::FieldRow*> rows_;
    // By role, or "name" and "" for the dialog.
    QHash<QString, QString> errors_;
    // The rows that hold two controls side by side.
    QBoxLayout* mergeRow_{nullptr};
    QBoxLayout* developerRow_{nullptr};
    QWidget* viewport_{nullptr};
    bool fitting_{false};
};

// #465: one more developer, never an existing conversation (those join by drag or menu).
class AddDeveloperForm final : public QWidget {
    Q_OBJECT

  public:
    AddDeveloperForm(TeamDirectory* teams, const QJsonObject& team, const QVariantList& models,
                     QWidget* parent = nullptr);

    [[nodiscard]] QJsonObject payload() const;
    bool validate();
    void submit(std::function<void()> added);
    [[nodiscard]] bool isFull() const;
    [[nodiscard]] QString hint() const;
    [[nodiscard]] ket::TextField* nameField() const { return name_; }
    [[nodiscard]] ket::TextField* laneField() const { return lane_; }
    [[nodiscard]] ket::TextField* pathsField() const { return paths_; }
    [[nodiscard]] QString error() const;
    void setAgent(const TeamAgent& agent);

    static ket::Dialog* present(QWidget* anchor, TeamDirectory* teams, const QJsonObject& team,
                                const QVariantList& models);

  private:
    TeamDirectory* teams_;
    QJsonObject team_;
    QVariantList models_;
    TeamAgent agent_;
    ket::TextField* name_;
    ket::TextField* lane_;
    ket::TextField* paths_;
    ket::Button* agentButton_;
    ket::FieldRow* nameRow_;
    QLabel* hint_;
    QLabel* error_;
};

} // namespace ketplus
