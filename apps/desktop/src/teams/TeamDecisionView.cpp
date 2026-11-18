#include "teams/TeamDecisionView.h"

#include "KetBadge.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPageParts.h"

#include <QDateTime>
#include <QJsonArray>
#include <QVBoxLayout>

namespace ketplus {
namespace {
using team_page::px;
using team_page::Text;

// The host validates the versioned record. Unknown versions stay visible as unavailable,
// never interpreted as a pass. This reader deliberately has no policy or mutation API.
bool knownRoute(const QJsonObject& d) {
    const QString route = d.value("route").toString();
    return d.value("version").toDouble(-1) == 1 &&
           (route == "leader_direct" || route == "developer_leader" ||
            route == "developer_reviewer");
}

QString bounded(const QJsonValue& value) { return value.toString().left(2000); }
} // namespace

bool TeamDecisionView::hasDetails(const QJsonObject& task) {
    return task.contains("routing_decision") || task.contains("review_findings") ||
           !task.value("review_block").toString().isEmpty() || task.value("round").toInt() > 0;
}

QString TeamDecisionView::actorName(const QJsonObject& team, const QJsonValue& actor) {
    if (actor.isObject()) {
        // Paired history uses the host's public actor shape rather than a session identifier.
        const auto who = actor.toObject();
        const QString role = who.value("role").toString();
        const QString name = role == "leader"      ? tr("Leader")
                             : role == "reviewer"  ? tr("Reviewer")
                             : role == "developer" ? tr("Developer")
                             : role == "user"      ? tr("You")
                             : role == "daemon"    ? tr("ketPlus")
                                                   : tr("Unknown actor");
        const QString number = who.value("number").toString();
        return number.isEmpty() ? name : tr("%1 #%2").arg(name, number.left(100));
    }
    const QString id = bounded(actor);
    if (id == "user")
        return tr("You");
    if (id == "daemon")
        return tr("ketPlus");
    const auto member = TeamDirectory::member(team, id);
    const QString name = member.value("name").toString();
    return !name.isEmpty() ? name : id.isEmpty() ? tr("Unknown actor") : id;
}

QString TeamDecisionView::routeLabel(const QJsonObject& d) {
    if (!knownRoute(d))
        return tr("Route unavailable");
    const QString route = d.value("route").toString();
    return route == "leader_direct"      ? tr("Leader answers directly")
           : route == "developer_leader" ? tr("Developer → leader")
                                         : tr("Developer → reviewer");
}

QString TeamDecisionView::summary(const QJsonObject& team, const QJsonObject& d) {
    return tr("%1 · Recorded by %2").arg(routeLabel(d), actorName(team, d.value("actor")));
}

QString TeamDecisionView::eventText(const QJsonObject& entry) {
    const QString kind = entry.value("kind").toString();
    const auto data = entry.value("data").toObject();
    bool unavailable = data.value("source_unavailable").toBool();
    for (const auto& value : data.value("changes").toArray()) {
        const auto change = value.toObject();
        unavailable |=
            change.value("field") == "worktree" &&
            (change.value("before") == "unreadable" || change.value("after") == "unreadable");
    }
    QString text;
    if (kind == "review_interrupted")
        text =
            unavailable
                ? tr("Review interrupted: source unavailable. No reviewer edit is inferred; no "
                     "content round counted.")
                : tr("Review interrupted. No content round counted; no reviewer edit is inferred.");
    else if (kind == "review_tampered")
        text = tr("Review invalidated: source changed. No content round counted.");
    else if (kind == "round_limit")
        text = tr("Content review limit reached. Leader arbitration is required.");
    else if (kind == "no_verdict")
        text = tr("The reviewer verdict is missing. No approval is inferred.");
    else
        return entry.value("text").toString();
    const int round = data.value("round").toInt();
    return round > 0 ? tr("Round %1 · %2").arg(round).arg(text) : text;
}

TeamDecisionView::TeamDecisionView(const QJsonObject& team, const QJsonObject& task,
                                   QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("teamDecisionTrail"));
    auto* box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(px(8));
    const auto line = [&](const QString& text, const char* name, bool muted = false) {
        auto* label = new Text(text, this);
        label->setObjectName(QLatin1String(name));
        if (muted) {
            label->setTone(QStringLiteral("text-secondary"));
            label->setType(12);
        }
        box->addWidget(label);
        return label;
    };
    auto* title = line(tr("Decision and review"), "teamDecisionTrailHeading");
    title->setType(14, QFont::Medium);
    const auto d = task.value("routing_decision").toObject();
    if (knownRoute(d)) {
        if (const QString id = bounded(d.value("task_id")); !id.isEmpty())
            line(tr("Task: %1").arg(id), "teamDecisionTaskId", true);
        auto* badge = new ket::Badge(routeLabel(d), ket::BadgeTone::Neutral, this);
        badge->setObjectName(QStringLiteral("teamDecisionRoute"));
        box->addWidget(badge, 0, Qt::AlignLeft);
        line(tr("Recorded by %1").arg(actorName(team, d.value("actor"))), "teamDecisionActor");
        line(tr("Historical leader decision. This is not a reviewer verdict or user approval."),
             "teamDecisionHistorical", true);
        const QString risk = d.value("risk").toString();
        line(risk == "low"        ? tr("Risk at decision: verified low risk")
             : risk == "advisory" ? tr("Advisory: no code change")
                                  : tr("Risk at decision: high or unknown"),
             "teamDecisionRisk");
        const QString gate = d.value("gate_reason").toString();
        QString reason =
            gate == "verified_low_risk"   ? tr("Bounded host evidence was verified.")
            : gate == "mandatory_review"  ? tr("Mandatory review is required.")
            : gate == "approval_required" ? tr("A required user approval is missing.")
            : gate == "unresolved_findings_or_failures"
                ? tr("Findings or failed checks remain unresolved.")
            : gate.startsWith("checks_") ? tr("Required checks are missing, failed or stale.")
            : gate == "risk_unknown"     ? tr("Risk coverage is incomplete.")
            : gate == "scope_or_rollback_unknown" ? tr("Scope or rollback evidence is missing.")
            : gate == "confidence_insufficient"   ? tr("Confidence is insufficient for this route.")
            : d.value("route").toString() == "leader_direct"
                ? tr("The leader can answer directly.")
                : tr("Reviewer verification is required.");
        line(reason, "teamDecisionGateReason");
        // The gate has a readable reason above; retain the machine codes in its tooltip,
        // rather than filling the decision flow with protocol identifiers.
        findChild<QLabel*>(QStringLiteral("teamDecisionGateReason"))
            ->setToolTip(bounded(d.value("reason")) + QStringLiteral(" · ") +
                         bounded(d.value("gate_reason")));
        const QString revision = bounded(d.value("revision"));
        line(tr("Assessed revision: %1").arg(revision), "teamDecisionRevision", true);
        const QString head = task.value("head_commit").toString();
        if (!head.isEmpty() && !revision.startsWith("team:") && head != revision)
            line(tr("The current head differs. This record does not verify the current change."),
                 "teamDecisionChangedHead");
        const auto refs = d.value("evidence_ids").toArray();
        line(tr("Recorded evidence references: %1").arg(refs.size()), "teamDecisionEvidence");
        for (qsizetype i = 0; i < qMin(refs.size(), qsizetype(33)); ++i)
            line(bounded(refs.at(i)), "teamDecisionEvidenceReference", true);
        // A route record contains references, not the individual check results or freshness.
        // Do not turn an old low-risk decision into today's green check marks.
        line(tr("Current check results are not included in this record."), "teamDecisionChecks",
             true);
        const QString follow = d.value("follow_up").toString();
        line(follow == "answer_only" ? tr("Next: answer directly.")
             : follow == "merge_then_acceptance"
                 ? tr("Next: merge, then verify acceptance through the current gates.")
                 : tr("Next: independent reviewer verification."),
             "teamDecisionFollowUp");
    } else if (task.contains("routing_decision")) {
        line(tr("This decision format is unavailable. No route approval is inferred."),
             "teamDecisionUnavailable");
    }
    if (const QString block = task.value("review_block").toString(); !block.isEmpty()) {
        line(
            eventText({{"kind", block},
                       {"text", tr("Review is blocked. Follow the task's current instructions.")}}),
            "teamDecisionReviewBlock");
    }
    const int round = task.value("round").toInt();
    if (round > 0) {
        const int limit = task.value("round_limit").toInt();
        line(limit > 0 ? tr("Content review round: %1 of %2").arg(round).arg(limit)
                       : tr("Content review round: %1").arg(round),
             "teamDecisionRounds", true);
        // Only a complete, ordered queue→handoff pair in the received history can establish
        // queue wait. Never infer it from a partial page, review duration or wall-clock now.
        QDateTime queued;
        qint64 wait = -1;
        for (const auto& value : task.value("history").toArray()) {
            const auto entry = value.toObject();
            const auto data = entry.value("data").toObject();
            const auto at = QDateTime::fromString(entry.value("at").toString(), Qt::ISODateWithMs);
            const QString kind = entry.value("kind").toString();
            if (kind == "state")
                queued = data.value("to").toString() == "in_review" ? at : QDateTime{};
            else if (kind == "review" && data.value("round").toInt() == round) {
                if (queued.isValid() && at.isValid() && queued <= at)
                    wait = queued.secsTo(at);
                queued = {};
            } else if (kind == "review_interrupted" || kind == "review_tampered")
                queued = {};
        }
        if (wait >= 0)
            line(tr("Last recorded review queue wait: %1 s").arg(wait), "teamDecisionQueueWait",
                 true);
    }
    const auto record = task.value("review_findings").toObject();
    if (task.contains("review_findings")) {
        line(tr("Reviewer findings"), "teamDecisionFindingsHeading");
        if (record.value("version").toDouble(-1) == 1 && record.value("summary_only").toBool() &&
            !record.value("legacy").toBool(true)) {
            line(tr("Finding details are available in the task. Closure is not inferred."),
                 "teamDecisionFindingsSummary", true);
        } else if (record.value("version").toDouble(-1) != 1 ||
                   record.value("legacy").toBool(true) || !record.value("findings").isArray()) {
            line(tr("Structured findings are unavailable. Read the reviewer report; closure is not "
                    "inferred."),
                 "teamDecisionLegacyFindings");
        } else {
            line(tr("Reviewer: %1").arg(actorName(team, record.value("reviewer"))),
                 "teamDecisionFindingsReviewer", true);
            for (const auto& value : record.value("findings").toArray()) {
                const auto f = value.toObject();
                const QString status = f.value("status").toString();
                const QString severity = f.value("severity").toString();
                const bool alwaysReview = severity != "p2" && severity != "nit";
                const bool preference = severity == "nit" &&
                                        !f.value("mandatory_rule").toBool(true) &&
                                        f.value("impact").toString() == "preference" &&
                                        (f.value("scope").toString() == "task" ||
                                         f.value("scope").toString() == "outside_task");
                const QString label = alwaysReview           ? tr("Unresolved — review required")
                                      : status == "fixed"    ? tr("Fix recorded")
                                      : status == "deferred" ? tr("Deferral recorded")
                                      : status == "non_blocking" && preference
                                          ? tr("Non-blocking preference")
                                          : tr("Unresolved — review required");
                line(
                    tr("%1 · %2 · %3")
                        .arg(bounded(f.value("id")), bounded(f.value("severity")).toUpper(), label),
                    "teamDecisionFinding");
                if (f.value("mandatory_rule").toBool(true))
                    line(tr("Mandatory requirement"), "teamDecisionFindingMandatory", true);
                line(tr("Close when: %1").arg(bounded(f.value("close_when"))),
                     "teamDecisionFindingCondition", true);
                if (f.value("owner").isObject() || !f.value("owner").toString().isEmpty())
                    line(tr("Follow-up owner: %1").arg(actorName(team, f.value("owner"))),
                         "teamDecisionFindingOwner", true);
                if (!f.value("evidence").toString().isEmpty())
                    line(tr("Evidence: %1").arg(bounded(f.value("evidence"))),
                         "teamDecisionFindingEvidence", true);
            }
            line(tr("Finding resolutions are audit records; they do not replace the reviewer "
                    "verdict."),
                 "teamDecisionResolutionNote", true);
        }
    }
    // Let spare viewport height collect below the content. AlignTop constrains the
    // layout to its unwrapped size hint and can clip height-for-width text.
    box->addStretch();
}
} // namespace ketplus
