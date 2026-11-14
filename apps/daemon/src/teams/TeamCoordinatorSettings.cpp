// ADR 0114, open point 2: the automatic-turn budget. The daemon keeps the global value every
// new team starts from; each team keeps its own, which the user changes while it runs.
#include "teams/TeamCoordinator.h"

#include "storage/LocalJournal.h"

namespace ketplus::daemon::teams {

QJsonObject TeamCoordinator::teamSettings() const {
    return {{QStringLiteral("turn_budget"), turnBudgetSetting()},
            {QStringLiteral("turn_budget_minimum"), minimumTurnBudget},
            {QStringLiteral("turn_budget_maximum"), maximumTurnBudget},
            {QStringLiteral("turn_budget_default"), defaultTurnBudget}};
}

int TeamCoordinator::turnBudgetSetting() const {
    int budget = defaultTurnBudget;
    // A stored value outside the range (a journal written by hand) counts as none.
    if (rules::validTurnBudget(journal_->teamSettings().value(QStringLiteral("turn_budget")),
                               &budget))
        return defaultTurnBudget;
    return budget;
}

std::optional<TeamError> TeamCoordinator::updateTeamSettings(const QJsonObject& changes) {
    QJsonObject settings = journal_->teamSettings();
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        if (it.key() != QStringLiteral("turn_budget"))
            return TeamError{QStringLiteral("invalid_payload"),
                             QStringLiteral("Teams have no setting %1.").arg(it.key())};
        int budget = 0;
        if (auto invalid = rules::validTurnBudget(it.value(), &budget))
            return invalid;
        settings.insert(it.key(), budget);
    }
    QString error;
    if (!journal_->saveTeamSettings(settings, &error))
        return TeamError{QStringLiteral("journal_error"),
                         error.isEmpty() ? QStringLiteral("The journal refused the change.")
                                         : error};
    return std::nullopt;
}

std::optional<TeamError> TeamCoordinator::changeTurnBudget(Team& team, const QJsonValue& value,
                                                           QList<Update>& updates) {
    int budget = 0;
    if (auto invalid = rules::validTurnBudget(value, &budget))
        return invalid;
    if (budget == team.turnBudget)
        return std::nullopt;
    updates.append(
        {.kind = QStringLiteral("turn_budget"),
         .text = say(team.language, QT_TRANSLATE_NOOP("Team", "Automatic turns: %1, %2 used."))
                     .arg(budget)
                     .arg(team.automaticTurns),
         .data = {{QStringLiteral("from"), team.turnBudget}, {QStringLiteral("to"), budget}}});
    team.turnBudget = budget;
    // The question "used its N automatic turns" no longer holds when turns are left: the team
    // runs again and its held messages go out (update() releases them).
    if (team.paused && team.pausedReason == QStringLiteral("budget") &&
        team.automaticTurns < budget) {
        team.paused = false;
        team.pausedReason.clear();
        for (Decision& decision : team.decisions)
            if (decision.kind == QStringLiteral("budget") &&
                decision.state == QStringLiteral("open")) {
                decision.state = QStringLiteral("withdrawn");
                decision.reason =
                    say(team.language,
                        QT_TRANSLATE_NOOP("Team", "The budget is now %1 automatic turns."))
                        .arg(budget);
                decision.answeredAt = nowIso();
            }
        updates.append(
            {.kind = QStringLiteral("resumed"),
             .text = say(team.language,
                         QT_TRANSLATE_NOOP("Team", "Automatic turns resumed: %1 of %2 used."))
                         .arg(team.automaticTurns)
                         .arg(budget)});
    }
    return std::nullopt;
}

} // namespace ketplus::daemon::teams
