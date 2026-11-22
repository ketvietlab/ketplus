#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QWidget>

#include <functional>

namespace ket {
class Banner;
class Checkbox;
class Choice;
class Dialog;
} // namespace ket

namespace ketplus {

class TeamAgentChooser;
class TeamDirectory;

namespace team_page {
class Text;
}

// T-3, Change provider…: one provider, model and effort for every member of a team at once, or
// for the members left checked, sent as team.provider.set (ADR 0114). Each member's next turn
// starts a fresh context on the new provider with the conversation so far as a transcript; a
// member running a turn switches when it ends, and nothing is interrupted. A reviewer runs on
// Codex only with Full access (ketplus#295): the user may give the team Full access with the
// switch, acknowledging the risk as Create does; otherwise the reviewer stays where it is.
class TeamProviderForm final : public QWidget {
    Q_OBJECT

  public:
    TeamProviderForm(TeamDirectory* teams, const QJsonObject& team, const QVariantList& models,
                     QWidget* parent = nullptr);

    [[nodiscard]] TeamAgentChooser* chooser() const { return chooser_; }
    [[nodiscard]] ket::Checkbox* allMembers() const { return all_; }
    [[nodiscard]] ket::Choice* member(const QString& sessionId) const {
        return members_.value(sessionId);
    }
    // Shown while a reviewer needs Full access: checking it gives the team Full access.
    [[nodiscard]] ket::Choice* grantAccess() const { return grant_; }
    // The members left checked, in the team's order.
    [[nodiscard]] QStringList chosen() const;
    // What Switch sends: the chosen members, without a reviewer that could not run on Codex.
    [[nodiscard]] QStringList switching() const;
    // A chosen reviewer moves to Codex while the team does not start members with Full access.
    [[nodiscard]] bool reviewerNeedsAccess() const;
    // The user gave the team Full access for that reviewer.
    [[nodiscard]] bool grantsFullAccess() const;
    [[nodiscard]] QJsonObject payload() const;
    // Sends the change; the dialog then shows each member's result in place of the form.
    void submit(std::function<void()> sent = {});
    [[nodiscard]] bool showsResults() const { return showsResults_; }
    [[nodiscard]] QString error() const { return error_; }

    static ket::Dialog* present(QWidget* anchor, TeamDirectory* teams, const QJsonObject& team,
                                const QVariantList& models);

  signals:
    void changed();
    void resultsShown();

  private:
    void describe();
    void showResults(const QJsonArray& results);

    TeamDirectory* teams_;
    QJsonObject team_;
    QVariantList models_;
    QWidget* fields_;
    TeamAgentChooser* chooser_;
    ket::Checkbox* all_;
    QHash<QString, ket::Choice*> members_;
    QStringList order_;
    ket::Banner* access_;
    ket::Choice* grant_;
    ket::Banner* summary_;
    ket::Banner* refusal_;
    QWidget* results_;
    bool showsResults_{false};
    QString sentProvider_;
    QString error_;
};

namespace team_provider {
// The provider the dialog starts on: away from the one most members run on, as the switch is
// usually to the other one.
[[nodiscard]] QString suggested(const QJsonObject& team, const QStringList& ready);
// One member's line under its name: role, provider and, while it runs, when it switches.
[[nodiscard]] QString memberLine(const QJsonObject& member);
// One member's result from team.provider.set, in words.
[[nodiscard]] QString resultLine(const QJsonObject& result, const QString& provider);
} // namespace team_provider

} // namespace ketplus
