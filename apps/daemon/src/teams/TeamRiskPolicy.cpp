#include "teams/TeamRiskPolicy.h"

#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

namespace ketplus::daemon::teams::risk {

QString routeName(const Route route) {
    switch (route) {
    case Route::LeaderDirect:
        return QStringLiteral("leader_direct");
    case Route::DeveloperLeader:
        return QStringLiteral("developer_leader");
    case Route::DeveloperReviewer:
        return QStringLiteral("developer_reviewer");
    }
    return QStringLiteral("developer_reviewer");
}

bool advisoryKind(const QString& kind) {
    return kind == QStringLiteral("explanation") || kind == QStringLiteral("proposal") ||
           kind == QStringLiteral("synthesis") || kind == QStringLiteral("coordination");
}

QStringList mandatoryDomains() {
    return {QStringLiteral("security"),          QStringLiteral("authentication"),
            QStringLiteral("secrets"),           QStringLiteral("billing"),
            QStringLiteral("data_deletion"),     QStringLiteral("migration"),
            QStringLiteral("automatic_cleanup"), QStringLiteral("signing"),
            QStringLiteral("updates_releases"),  QStringLiteral("protocol_compatibility"),
            QStringLiteral("large_blast_radius")};
}

bool evidenceId(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    return pattern.match(value).hasMatch();
}

Evaluation evaluate(const VerifiedEvidence* evidence, const int confidence) {
    const auto reviewer = [](const QString& reason) {
        return Evaluation{Route::DeveloperReviewer, reason};
    };
    static const QRegularExpression commit(QStringLiteral("\\A[0-9a-f]{40}\\z"));
    if (!evidence || !evidenceId(evidence->id) || evidence->teamId <= 0 ||
        evidence->taskId.isEmpty() || evidence->taskRevision < 0 ||
        !commit.match(evidence->revision).hasMatch())
        return reviewer(QStringLiteral("evidence_missing"));
    if (evidence->domains.size() != mandatoryDomains().size())
        return reviewer(QStringLiteral("risk_unknown"));
    for (const QString& domain : mandatoryDomains()) {
        const Signal signal = evidence->domains.value(domain, Signal::Unknown);
        if (signal == Signal::RequiresReview)
            return reviewer(QStringLiteral("mandatory_review"));
        if (signal != Signal::Clear)
            return reviewer(QStringLiteral("risk_unknown"));
    }
    if (!evidence->materialRiskKnown || !evidence->lowRiskVerified)
        return reviewer(QStringLiteral("risk_unknown"));
    if (evidence->unresolvedFindings != 0 || evidence->unresolvedP0 != 0 ||
        evidence->unresolvedP1 != 0 || evidence->unresolvedFailures != 0)
        return reviewer(QStringLiteral("unresolved_findings_or_failures"));
    if (!evidence->userApprovalsSatisfied)
        return reviewer(QStringLiteral("approval_required"));
    if (!evidenceId(evidence->scopeEvidence) || !evidenceId(evidence->rollbackEvidence))
        return reviewer(QStringLiteral("scope_or_rollback_unknown"));
    if (evidence->requiredChecks.isEmpty() || evidence->requiredChecks.size() > 32 ||
        evidence->checks.size() > 32)
        return reviewer(QStringLiteral("checks_missing"));
    QSet<QString> required;
    for (const QString& name : evidence->requiredChecks) {
        if (name.isEmpty() || name.size() > 100 || required.contains(name))
            return reviewer(QStringLiteral("checks_invalid"));
        required.insert(name);
    }
    QSet<QString> passed;
    for (const Check& check : evidence->checks) {
        if (!check.passed || check.revision != evidence->revision ||
            !evidenceId(check.evidenceId) || check.name.isEmpty() || passed.contains(check.name))
            return reviewer(QStringLiteral("checks_failed_or_stale"));
        passed.insert(check.name);
    }
    if (!required.subtract(passed).isEmpty())
        return reviewer(QStringLiteral("checks_missing"));
    if (confidence <= 90 || confidence > 100)
        return reviewer(QStringLiteral("confidence_insufficient"));
    return {Route::DeveloperLeader, QStringLiteral("verified_low_risk")};
}

QJsonObject toJson(const Decision& d) {
    return {{QStringLiteral("version"), 1},
            {QStringLiteral("team_id"), QString::number(d.teamId)},
            {QStringLiteral("task_id"), d.taskId},
            {QStringLiteral("decision_id"),
             QStringLiteral("routing:%1:%2").arg(d.teamId).arg(d.teamRevision)},
            {QStringLiteral("route"), routeName(d.route)},
            {QStringLiteral("risk"), d.risk},
            {QStringLiteral("kind"), d.kind},
            {QStringLiteral("gate_reason"), d.gateReason},
            {QStringLiteral("confidence"), d.confidence},
            {QStringLiteral("reason"), d.reason},
            {QStringLiteral("actor"), d.actor},
            {QStringLiteral("revision"), d.revision},
            {QStringLiteral("team_revision"), d.teamRevision},
            {QStringLiteral("task_revision"), d.taskRevision},
            {QStringLiteral("at"), d.at},
            {QStringLiteral("evidence_ids"), QJsonArray::fromStringList(d.evidenceIds)},
            {QStringLiteral("scope_evidence"), d.scopeEvidence},
            {QStringLiteral("rollback_evidence"), d.rollbackEvidence},
            {QStringLiteral("follow_up"), d.followUp}};
}

std::optional<Decision> fromJson(const QJsonValue& value) {
    if (!value.isObject())
        return std::nullopt;
    const QJsonObject j = value.toObject();
    if (j.value("version").toInt(-1) != 1)
        return std::nullopt;
    Decision d;
    d.teamId = j.value("team_id").toString().toLongLong();
    d.taskId = j.value("task_id").toString();
    const QString route = j.value("route").toString();
    if (route == routeName(Route::LeaderDirect))
        d.route = Route::LeaderDirect;
    else if (route == routeName(Route::DeveloperLeader))
        d.route = Route::DeveloperLeader;
    else if (route != routeName(Route::DeveloperReviewer))
        return std::nullopt;
    d.risk = j.value("risk").toString();
    d.kind = j.value("kind").toString();
    d.gateReason = j.value("gate_reason").toString();
    d.confidence = j.value("confidence").toInt(-1);
    d.reason = j.value("reason").toString();
    d.actor = j.value("actor").toString();
    d.revision = j.value("revision").toString();
    d.teamRevision = j.value("team_revision").toInteger(-1);
    d.taskRevision = j.value("task_revision").toInteger(-1);
    d.at = j.value("at").toString();
    d.scopeEvidence = j.value("scope_evidence").toString();
    d.rollbackEvidence = j.value("rollback_evidence").toString();
    d.followUp = j.value("follow_up").toString();
    const auto refs = j.value("evidence_ids");
    if (!refs.isArray() || refs.toArray().size() > 33)
        return std::nullopt;
    for (const auto& item : refs.toArray()) {
        if (!item.isString() || !evidenceId(item.toString()))
            return std::nullopt;
        d.evidenceIds.append(item.toString());
    }
    static const QRegularExpression code(QStringLiteral("\\A[a-z][a-z0-9_]{0,99}\\z"));
    static const QRegularExpression commit(QStringLiteral("\\A[0-9a-f]{40}\\z"));
    if (d.teamId < 1 || d.teamRevision < 1 ||
        j.value("decision_id").toString() !=
            QStringLiteral("routing:%1:%2").arg(d.teamId).arg(d.teamRevision) ||
        d.actor.isEmpty() || d.actor.size() > 200 || !code.match(d.reason).hasMatch() ||
        !code.match(d.gateReason).hasMatch() || !d.at.endsWith(QLatin1Char('Z')) ||
        !QDateTime::fromString(d.at, Qt::ISODateWithMs).isValid())
        return std::nullopt;
    if (d.route == Route::LeaderDirect) {
        if (!d.taskId.isEmpty() || !advisoryKind(d.kind) || d.risk != QStringLiteral("advisory") ||
            d.followUp != QStringLiteral("answer_only") || !d.revision.startsWith("team:") ||
            d.revision != QStringLiteral("team:%1:%2").arg(d.teamId).arg(d.teamRevision - 1) ||
            !d.evidenceIds.isEmpty() || d.taskRevision != -1 || d.confidence != -1)
            return std::nullopt;
    } else {
        if (d.taskId.isEmpty() || d.taskId.size() > 100 || d.kind != QStringLiteral("change") ||
            !commit.match(d.revision).hasMatch() || d.taskRevision < 0)
            return std::nullopt;
        if (d.route == Route::DeveloperLeader) {
            if (d.evidenceIds.isEmpty() || !evidenceId(d.scopeEvidence) ||
                !evidenceId(d.rollbackEvidence) || d.risk != QStringLiteral("low") ||
                d.followUp != QStringLiteral("merge_then_acceptance") ||
                d.gateReason != QStringLiteral("verified_low_risk") || d.confidence <= 90 ||
                d.confidence > 100)
                return std::nullopt;
        } else if ((d.risk != QStringLiteral("high_or_unknown") &&
                    d.risk != QStringLiteral("low")) ||
                   d.followUp != QStringLiteral("review_required"))
            return std::nullopt;
    }
    return d;
}

} // namespace ketplus::daemon::teams::risk
