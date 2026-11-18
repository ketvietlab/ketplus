#include "teams/TeamReviewPolicy.h"
#include "teams/TeamRiskPolicy.h"
#include <QJsonDocument>
#include <QJsonParseError>

#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace ketplus::daemon::teams::review_policy {
namespace {
bool oneOf(const QString& value, const QStringList& choices) { return choices.contains(value); }
QJsonObject findingJson(const Finding& f) {
    return {{"id", f.id},
            {"severity", f.severity},
            {"impact", f.impact},
            {"scope", f.scope},
            {"evidence", f.evidence},
            {"close_when", f.closeWhen},
            {"mandatory_rule", f.mandatoryRule},
            {"status", f.status},
            {"actor", f.actor},
            {"reason", f.reason},
            {"owner", f.owner},
            {"resolved_revision", f.resolvedRevision},
            {"receipt", f.receipt},
            {"at", f.at}};
}
std::optional<Finding> finding(const QJsonObject& j, bool persisted) {
    const QSet<QString> input{"id",       "severity",   "impact",        "scope",
                              "evidence", "close_when", "mandatory_rule"};
    const QSet<QString> resolution{"status",  "actor", "reason", "owner", "resolved_revision",
                                   "receipt", "at"};
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!input.contains(it.key()) && (!persisted || !resolution.contains(it.key())))
            return std::nullopt;
    Finding f;
    f.id = j.value("id").toString();
    f.severity = j.value("severity").toString();
    f.impact = j.value("impact").toString();
    f.scope = j.value("scope").toString();
    f.evidence = j.value("evidence").toString();
    f.closeWhen = j.value("close_when").toString();
    if (!code(f.id) || !code(f.closeWhen) || !risk::evidenceId(f.evidence) ||
        !oneOf(f.severity, {"p0", "p1", "p2", "nit", "unknown"}) ||
        !oneOf(f.impact, {"requirement", "required_check", "behavior", "preference", "unknown"}) ||
        !oneOf(f.scope, {"task", "outside_task", "unknown"}) || !j.value("mandatory_rule").isBool())
        return std::nullopt;
    f.mandatoryRule = j.value("mandatory_rule").toBool();
    if (!persisted) {
        if (f.severity == "nit" && !f.mandatoryRule && f.impact == "preference" &&
            f.scope != "unknown")
            f.status = QStringLiteral("non_blocking");
        return f;
    }
    f.status = j.value("status").toString();
    f.actor = j.value("actor").toString();
    f.reason = j.value("reason").toString();
    f.owner = j.value("owner").toString();
    f.resolvedRevision = j.value("resolved_revision").toString();
    f.receipt = j.value("receipt").toString();
    f.at = j.value("at").toString();
    if (f.status == "open") {
        if (!f.actor.isEmpty() || !f.reason.isEmpty() || !f.owner.isEmpty() ||
            !f.resolvedRevision.isEmpty() || !f.receipt.isEmpty() || !f.at.isEmpty())
            return std::nullopt;
    } else if (f.status == "non_blocking") {
        if (f.severity != "nit" || f.mandatoryRule || f.impact != "preference" ||
            f.scope == "unknown")
            return std::nullopt;
    } else if (f.status == "fixed" || f.status == "deferred") {
        if (!leaderVerifiable(f) || f.actor.isEmpty() || f.actor.size() > 200 || !code(f.reason) ||
            !revision(f.resolvedRevision) || !risk::evidenceId(f.receipt) || !f.at.endsWith('Z') ||
            !QDateTime::fromString(f.at, Qt::ISODateWithMs).isValid() ||
            (f.status == "deferred" &&
             (!deferrable(f) || f.owner.isEmpty() || f.owner.size() > 200)))
            return std::nullopt;
    } else
        return std::nullopt;
    return f;
}
std::optional<QList<Finding>> findings(const QJsonValue& value, bool persisted) {
    if (!value.isArray() || value.toArray().size() > 32)
        return std::nullopt;
    QList<Finding> result;
    QSet<QString> ids;
    for (const auto& item : value.toArray()) {
        if (!item.isObject())
            return std::nullopt;
        auto f = finding(item.toObject(), persisted);
        if (!f || ids.contains(f->id))
            return std::nullopt;
        ids.insert(f->id);
        result.append(*f);
    }
    return result;
}
} // namespace
bool code(const QString& value) {
    static const QRegularExpression p(QStringLiteral("\\A[a-z][a-z0-9_]{0,99}\\z"));
    return p.match(value).hasMatch();
}
bool revision(const QString& value) {
    static const QRegularExpression p(QStringLiteral("\\A[0-9a-f]{40}\\z"));
    return p.match(value).hasMatch();
}
bool deferrable(const Finding& f) {
    return f.severity == "p2" && !f.mandatoryRule && f.scope == "outside_task" &&
           (f.impact == "behavior" || f.impact == "preference");
}
bool leaderVerifiable(const Finding& f) {
    return (f.severity == "p2" || f.severity == "nit") && f.scope != "unknown" &&
           f.impact != "unknown";
}
bool blocks(const Finding& f) {
    if (f.severity == "p0" || f.severity == "p1" || f.severity == "unknown")
        return true;
    if (f.status == "fixed" || f.status == "deferred")
        return false;
    return f.status != "non_blocking" || f.severity != "nit" || f.mandatoryRule ||
           f.impact != "preference" || f.scope == "unknown";
}
bool blocks(const Record& r) {
    return r.legacy || std::any_of(r.findings.cbegin(), r.findings.cend(),
                                   [](const Finding& f) { return blocks(f); });
}
QJsonObject toJson(const Record& r) {
    QJsonArray rows;
    for (const Finding& f : r.findings)
        rows.append(findingJson(f));
    return {{"version", 1},     {"revision", r.revision}, {"reviewer", r.reviewer},
            {"round", r.round}, {"legacy", r.legacy},     {"findings", rows}};
}
std::optional<Record> fromJson(const QJsonValue& value) {
    if (!value.isObject())
        return std::nullopt;
    const auto j = value.toObject();
    auto rows = findings(j.value("findings"), true);
    Record r;
    r.revision = j.value("revision").toString();
    r.reviewer = j.value("reviewer").toString();
    r.round = j.value("round").toInt(-1);
    if ((!j.value("version").isDouble() || j.value("version").toDouble() != 1) ||
        (!revision(r.revision) && !(j.value("legacy").toBool() && r.revision.isEmpty())) ||
        r.reviewer.isEmpty() || r.reviewer.size() > 200 || r.round < 1 || r.round > 1000000 ||
        !j.value("round").isDouble() || j.value("round").toDouble() != r.round ||
        !j.value("legacy").isBool() || !rows)
        return std::nullopt;
    r.legacy = j.value("legacy").toBool();
    r.findings = *rows;
    return r;
}
std::optional<QList<Finding>> report(const QJsonObject& j) {
    if (j.size() != 2 || (!j.value("version").isDouble() || j.value("version").toDouble() != 1))
        return std::nullopt;
    return findings(j.value("findings"), false);
}
std::optional<Record> reply(const QString& text, const QString& head, const QString& reviewer,
                            const int round, const bool findingsVerdict) {
    const QString marker = QStringLiteral("```ketplus-findings-v1\n");
    const qsizetype begin = text.indexOf(marker);
    if (begin < 0 && !findingsVerdict)
        return std::nullopt;
    Record result{.revision = revision(head) ? head : QString{},
                  .reviewer = reviewer,
                  .round = round,
                  .legacy = true};
    if (begin < 0 || text.indexOf(marker, begin + marker.size()) >= 0)
        return result;
    const qsizetype end = text.indexOf(QStringLiteral("\n```"), begin + marker.size());
    if (end < 0 || end - begin > 32768 || !revision(head))
        return result;
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(
        text.mid(begin + marker.size(), end - begin - marker.size()).toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return result;
    const auto rows = report(doc.object());
    if (!rows || (findingsVerdict && rows->isEmpty()))
        return result;
    result.legacy = false;
    result.findings = *rows;
    return result;
}

std::optional<Request> request(const QString& json) {
    if (json.toUtf8().size() > 16384)
        return std::nullopt;
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;
    const auto j = doc.object();
    const QSet<QString> fields{"version",
                               "action",
                               "expected_team_revision",
                               "expected_task_revision",
                               "head_commit",
                               "evidence_id",
                               "resolutions",
                               "confidence"};
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!fields.contains(it.key()))
            return std::nullopt;
    if (!j.value("version").isDouble() || j.value("version").toDouble() != 1)
        return std::nullopt;
    Request r;
    r.action = j.value("action").toString();
    r.head = j.value("head_commit").toString();
    r.receipt = j.value("evidence_id").toString();
    r.teamRevision = j.value("expected_team_revision").toInteger(-1);
    r.taskRevision = j.value("expected_task_revision").toInteger(-1);
    for (const auto& key :
         {QStringLiteral("expected_team_revision"), QStringLiteral("expected_task_revision")}) {
        const auto v = j.value(key);
        if (!v.isDouble() || v.toInteger(-1) < 0 ||
            v.toDouble() != static_cast<double>(v.toInteger(-1)))
            return std::nullopt;
    }
    if (!revision(r.head))
        return std::nullopt;
    if (r.action == "request_review") {
        if (j.contains("evidence_id") || j.contains("resolutions") || j.contains("confidence"))
            return std::nullopt;
        return r;
    }
    const auto confidence = j.value("confidence");
    r.confidence = confidence.toInt(-1);
    if (!confidence.isDouble() || confidence.toDouble() != r.confidence || r.confidence < 0 ||
        r.confidence > 100)
        return std::nullopt;
    if (r.action != "resolve_findings" || !risk::evidenceId(r.receipt) ||
        !j.value("resolutions").isArray())
        return std::nullopt;
    const auto rows = j.value("resolutions").toArray();
    if (rows.isEmpty() || rows.size() > 32)
        return std::nullopt;
    QSet<QString> seen;
    for (const auto& value : rows) {
        if (!value.isObject())
            return std::nullopt;
        const auto row = value.toObject();
        for (auto it = row.begin(); it != row.end(); ++it)
            if (it.key() != "id" && it.key() != "status" && it.key() != "reason" &&
                it.key() != "owner")
                return std::nullopt;
        Resolution x{row.value("id").toString(), row.value("status").toString(),
                     row.value("reason").toString(), row.value("owner").toString()};
        if (!code(x.id) || !code(x.reason) || seen.contains(x.id) ||
            (x.status != "fixed" && x.status != "deferred") ||
            (x.status == "deferred" ? !code(x.owner) : row.contains("owner")))
            return std::nullopt;
        seen.insert(x.id);
        r.resolutions.append(x);
    }
    return r;
}
std::optional<Record> resolve(const Record& record, const Request& request,
                              const risk::VerifiedEvidence& receipt, const QString& actor,
                              const QString& at) {
    if (request.action != "resolve_findings" || request.resolutions.isEmpty() || record.legacy ||
        record.findings.isEmpty() || actor.isEmpty() ||
        receipt.reviewedRevision != record.revision || !revision(record.revision) ||
        receipt.revision != request.head || receipt.id != request.receipt ||
        receipt.taskRevision != request.taskRevision ||
        risk::evaluate(&receipt, request.confidence).route != risk::Route::DeveloperLeader)
        return std::nullopt;
    Record result = record;
    QSet<QString> seen;
    for (const auto& change : request.resolutions) {
        auto f = std::find_if(result.findings.begin(), result.findings.end(),
                              [&](const Finding& f) { return f.id == change.id; });
        if (f == result.findings.end() || f->status != "open" || !leaderVerifiable(*f) ||
            (change.status == "fixed" && !receipt.localFixVerified) ||
            (change.status == "deferred" &&
             (!deferrable(*f) || f->impact != "preference" ||
              f->closeWhen != "owner_accepts_outside_preference" || receipt.localFixVerified ||
              receipt.reviewedRevision != receipt.revision ||
              !receipt.requiredChecks.contains(
                  QStringLiteral("reviewed_preference_nonblocking")))) ||
            seen.contains(change.id))
            return std::nullopt;
        seen.insert(change.id);
        f->status = change.status;
        f->actor = actor;
        f->reason = change.reason;
        f->owner = change.owner;
        f->resolvedRevision = request.head;
        f->receipt = receipt.id;
        f->at = at;
    }
    if (blocks(result))
        return std::nullopt;
    // Reuse the persistence validator to reject malformed audit/time fields as well.
    return fromJson(toJson(result));
}
} // namespace ketplus::daemon::teams::review_policy
