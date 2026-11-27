#pragma once

#include <QJsonObject>
#include <QString>
#include <QWidget>

#include <functional>

namespace ket {
class Dialog;
class FieldRow;
class TextField;
} // namespace ket

namespace ketplus {

class TeamDirectory;

// ADR 0114, open point 2: the automatic-turn budget. The daemon keeps the global value a new
// team starts from (Settings → Teams); each team keeps its own, changed while it runs.
namespace team_budget {

// The daemon's range (TeamRules validTurnBudget) and its value until the user changes it.
inline constexpr int fewest = 5;
inline constexpr int most = 200;
inline constexpr int standard = 40;

// Empty when `text` is a whole number in the range, which goes to `value`; otherwise the
// daemon's refusal in the same words.
[[nodiscard]] QString error(const QString& text, int* value = nullptr);
// The field the three places share: digits only, as wide as three of them need.
[[nodiscard]] ket::TextField* field(const QString& accessibleName, QWidget* parent);
// Reads the global value; a daemon without teams.settings/1 answers with the standard 40.
void readGlobal(TeamDirectory* teams, QObject* context, std::function<void(int)> done);
// "12 of 40 automatic turns used" for a team's read model.
[[nodiscard]] QString usage(const QJsonObject& team);

} // namespace team_budget

// A team's own budget, changed while it runs. The change applies from the next automatic
// turn and the turns used stay counted; one that leaves turns resumes a team the budget paused.
class TeamTurnBudgetForm final : public QWidget {
    Q_OBJECT

  public:
    TeamTurnBudgetForm(TeamDirectory* teams, const QJsonObject& team, QWidget* parent = nullptr);

    [[nodiscard]] ket::TextField* budgetField() const { return field_; }
    [[nodiscard]] QString error() const { return error_; }
    // Checks the value and sends team.update; `saved` runs once the daemon took it.
    void submit(std::function<void()> saved);

    static ket::Dialog* present(QWidget* anchor, TeamDirectory* teams, const QJsonObject& team);

  private:
    void showError(const QString& message);

    TeamDirectory* teams_;
    QString teamId_;
    int budget_;
    ket::TextField* field_;
    ket::FieldRow* row_;
    QString error_;
};

} // namespace ketplus
