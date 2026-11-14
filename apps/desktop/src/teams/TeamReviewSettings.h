#pragma once

// T-40 (ADR 0114 §2): how a team reviews, in New team and in the team's Review settings.
// T-53: Max review passes per PR, one to five (two by default), before the leader decides on
// findings left, and where the review is written: on the board, or as
// a comment on the task's pull request with the board keeping the verdict and the link. The
// daemon checks both and applies a change to tasks that enter review afterwards.
#include <QJsonObject>
#include <QString>
#include <QWidget>

namespace ket {
class Dialog;
class FieldRow;
class Radio;
class Select;
} // namespace ket

namespace ketplus {

class TeamDirectory;

namespace team_review {

inline constexpr int standardRounds = 2;
inline constexpr int maximumRounds = 5;
inline const QString board = QStringLiteral("board");
inline const QString pullRequest = QStringLiteral("pull_request");

// "2 review passes on the board", for the rounds and place of `team`'s read model.
[[nodiscard]] QString summary(int rounds, const QString& place);
[[nodiscard]] QString summary(const QJsonObject& team);
// The team's values as the daemon keeps them; the standard ones for an older daemon.
[[nodiscard]] int rounds(const QJsonObject& team);
[[nodiscard]] QString place(const QJsonObject& team);

} // namespace team_review

// The two rows. `note` follows the place's help in the team's settings: when a change applies.
class TeamReviewFields final : public QWidget {
    Q_OBJECT

  public:
    TeamReviewFields(int rounds, const QString& place, const QString& note = {},
                     QWidget* parent = nullptr);

    [[nodiscard]] int rounds() const;
    [[nodiscard]] QString place() const;
    void setValues(int rounds, const QString& place);

  private:
    void updateHelp();

    QString note_;
    ket::Select* rounds_;
    ket::Radio* board_;
    ket::Radio* pullRequest_;
    ket::FieldRow* roundsRow_;
    ket::FieldRow* placeRow_;
};

// The team's Review settings: sends team.update with the rounds and the place, and shows the
// daemon's refusal in its words; closes once the daemon took the change.
[[nodiscard]] ket::Dialog* teamReviewSettings(QWidget* anchor, TeamDirectory* teams,
                                              const QJsonObject& team);

} // namespace ketplus
