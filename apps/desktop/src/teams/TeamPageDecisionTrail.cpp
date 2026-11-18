#include "teams/TeamDecisionView.h"
#include "teams/TeamPage.h"
#include "teams/TeamPageParts.h"

#include "KetButton.h"
#include <QJsonArray>
#include <QVBoxLayout>

namespace ketplus {

void TeamPage::buildDecisionTrail(const QJsonObject& team) {
    const auto advisory = team.value("last_routing_decision").toObject();
    QList<QJsonObject> tasks;
    for (const auto& value : team.value("tasks").toArray()) {
        const auto task = value.toObject();
        if (TeamDecisionView::hasDetails(task))
            tasks.append(task);
    }
    if (advisory.isEmpty() && tasks.isEmpty())
        return; // Legacy teams keep their existing layout.
    addSection(tr("Decision trail"), {}, "teamDecisionTrailSection");
    auto* panel = new team_page::Panel(column_);
    panel->setObjectName(QStringLiteral("teamDecisionTrailPanel"));
    auto* rows = new QVBoxLayout(panel);
    rows->setContentsMargins(1, 1, 1, 1);
    rows->setSpacing(0);
    auto* noteRow = new team_page::Row(panel, false);
    auto* noteLayout = new QVBoxLayout(noteRow);
    noteLayout->setContentsMargins(team_page::px(18), team_page::px(12), team_page::px(18),
                                   team_page::px(12));
    auto* note = new team_page::Text(
        tr("Historical records. Current task state and reviewer verdicts remain separate."),
        noteRow);
    note->setObjectName(QStringLiteral("teamDecisionTrailHistorical"));
    note->setTone(QStringLiteral("text-secondary"));
    note->setType(12);
    noteLayout->addWidget(note);
    rows->addWidget(noteRow);
    if (!advisory.isEmpty()) {
        auto* row = new team_page::Row(panel, false);
        auto* box = new QVBoxLayout(row);
        box->setContentsMargins(team_page::px(18), team_page::px(12), team_page::px(18),
                                team_page::px(12));
        box->addWidget(new TeamDecisionView(team, {{"routing_decision", advisory}}, row));
        rows->addWidget(row);
    }
    for (qsizetype index = 0; index < tasks.size(); ++index) {
        const auto task = tasks.at(index);
        const QString id = task.value("id").toString();
        auto* row = new team_page::Row(panel, index > 0 || !advisory.isEmpty());
        auto* box = new QVBoxLayout(row);
        box->setContentsMargins(team_page::px(18), team_page::px(12), team_page::px(18),
                                team_page::px(12));
        box->setSpacing(team_page::px(4));
        const auto decision = task.value("routing_decision").toObject();
        const QString summary =
            decision.isEmpty() ? tr("Review details") : TeamDecisionView::summary(team, decision);
        auto* description = new team_page::Text(tr("%1 · %2").arg(id, summary), row);
        description->setObjectName(QStringLiteral("teamDecisionTaskSummary"));
        box->addWidget(description);
        auto* open = new ket::Button(tr("Open task"), ket::ButtonVariant::Ghost, row);
        open->setObjectName(QStringLiteral("teamDecisionTaskOpen"));
        open->setAccessibleName(tr("Open decision and review for %1").arg(id));
        open->setToolTip(tr("%1 · %2").arg(id, summary));
        connect(open, &QAbstractButton::clicked, this, [this, id] { openTask(id); });
        box->addWidget(open, 0, Qt::AlignLeft);
        const QString block = task.value("review_block").toString();
        if (!block.isEmpty()) {
            auto* text = new team_page::Text(
                TeamDecisionView::eventText(
                    {{"kind", block},
                     {"text",
                      tr("Review is blocked. Open the task for its current instructions.")}}),
                row);
            text->setObjectName(QStringLiteral("teamDecisionTaskBlock"));
            box->addWidget(text);
        }
        rows->addWidget(row);
    }
    layout_->addWidget(panel);
}
} // namespace ketplus
