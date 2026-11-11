// ADR 0116: team commands from a paired phone. The cloud layer opens and scopes them; here
// each one is held to the fields the phone may send, then runs through teamCommand, the
// path a local client's request takes, and its answer is projected for the phone.
#include "runtime/SessionHost.h"

#include "teams/TeamRemoteView.h"

#include <QPointer>
#include <QSet>

namespace ketplus::daemon {
namespace {

QJsonObject refusal(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

QJsonObject invalid(const QString& message) {
    return refusal(QStringLiteral("invalid_payload"), message);
}

const QHash<QString, QSet<QString>>& allowedFields() {
    static const QHash<QString, QSet<QString>> fields{
        {QStringLiteral("team.list"), {}},
        {QStringLiteral("team.get"), {QStringLiteral("team_id"), QStringLiteral("known_revision")}},
        {QStringLiteral("team.task.get"),
         {QStringLiteral("team_id"), QStringLiteral("task_id"), QStringLiteral("history_limit"),
          QStringLiteral("history_before")}},
        {QStringLiteral("team.updates.list"),
         {QStringLiteral("team_id"), QStringLiteral("before_id"), QStringLiteral("limit")}},
        {QStringLiteral("team.decision.list"),
         {QStringLiteral("team_id"), QStringLiteral("state"), QStringLiteral("before_id"),
          QStringLiteral("limit")}},
        {QStringLiteral("team.decision.answer"),
         {QStringLiteral("team_id"), QStringLiteral("decision_id"), QStringLiteral("choice"),
          QStringLiteral("text"), QStringLiteral("expected_revision")}},
        {QStringLiteral("team.update"),
         {QStringLiteral("team_id"), QStringLiteral("paused"), QStringLiteral("merge_policy"),
          QStringLiteral("expected_revision")}},
        {QStringLiteral("team.delete"),
         {QStringLiteral("team_id"), QStringLiteral("confirmation"),
          QStringLiteral("expected_revision")}},
        // `repository` is the path the cloud layer resolved from the envelope's repositoryId.
        {QStringLiteral("team.create"),
         {QStringLiteral("repository"), QStringLiteral("name"), QStringLiteral("goal"),
          QStringLiteral("integration_branch"), QStringLiteral("merge_policy"),
          QStringLiteral("leader_writes_requirements"), QStringLiteral("execution_policy"),
          QStringLiteral("risk_acknowledged"), QStringLiteral("plan")}},
        {QStringLiteral("team.member.add"),
         {QStringLiteral("team_id"), QStringLiteral("name"), QStringLiteral("lane"),
          QStringLiteral("paths"), QStringLiteral("provider"), QStringLiteral("model"),
          QStringLiteral("effort"), QStringLiteral("expected_revision")}},
        {QStringLiteral("team.member.remove"),
         {QStringLiteral("team_id"), QStringLiteral("session_id"),
          QStringLiteral("expected_revision")}},
        {QStringLiteral("team.member.join"),
         {QStringLiteral("team_id"), QStringLiteral("session_id"), QStringLiteral("lane"),
          QStringLiteral("paths"), QStringLiteral("expected_revision")}},
        {QStringLiteral("team.task.handover"),
         {QStringLiteral("team_id"), QStringLiteral("task_id"), QStringLiteral("owner"),
          QStringLiteral("reason"), QStringLiteral("expected_revision")}}};
    return fields;
}

bool pageLimit(const QJsonObject& payload, const QString& key) {
    const QJsonValue value = payload.value(key);
    return value.isUndefined() || (value.isDouble() && value.toInteger() >= 1 &&
                                   value.toInteger() <= teams::remote::maximumPage);
}

// The fields of §4 and their bounds; nothing when the payload may run.
std::optional<QJsonObject> payloadRefusal(const QString& type, const QJsonObject& payload) {
    const QSet<QString>& allowed = allowedFields().value(type);
    for (auto it = payload.constBegin(); it != payload.constEnd(); ++it)
        if (!allowed.contains(it.key()))
            return invalid(
                QStringLiteral("A paired device cannot send %1 with %2.").arg(type, it.key()));
    if (type != QStringLiteral("team.list") && type != QStringLiteral("team.create") &&
        !payload.value(QStringLiteral("team_id")).isString())
        return invalid(QStringLiteral("Name the team."));
    if (!SessionHost::isRemoteTeamRead(type) && type != QStringLiteral("team.create") &&
        (!payload.value(QStringLiteral("expected_revision")).isDouble() ||
         payload.value(QStringLiteral("expected_revision")).toInteger() < 0))
        return invalid(QStringLiteral("A change names the team revision it was drawn from."));
    if (!pageLimit(payload, QStringLiteral("limit")) ||
        !pageLimit(payload, QStringLiteral("history_limit")))
        return invalid(QStringLiteral("A page holds 1 to 50 entries."));
    if (type == QStringLiteral("team.decision.answer")) {
        const bool choice = payload.contains(QStringLiteral("choice"));
        const QString text = payload.value(QStringLiteral("text")).toString();
        if (choice == payload.contains(QStringLiteral("text")) ||
            (!choice && (text.trimmed().isEmpty() || text.size() > 4000)))
            return invalid(QStringLiteral("Answer with one of the choices, or 1 to 4,000 "
                                          "characters."));
    }
    if (type == QStringLiteral("team.update")) {
        const QJsonValue paused = payload.value(QStringLiteral("paused"));
        const QJsonValue policy = payload.value(QStringLiteral("merge_policy"));
        if ((paused.isUndefined() && policy.isUndefined()) ||
            (!paused.isUndefined() && !paused.isBool()) ||
            (!policy.isUndefined() && policy != QStringLiteral("leader") &&
             policy != QStringLiteral("user")))
            return invalid(QStringLiteral("A paired device pauses, resumes or chooses who "
                                          "merges."));
    }
    if (type == QStringLiteral("team.delete") &&
        payload.value(QStringLiteral("confirmation")).toString() != QStringLiteral("DELETE"))
        return refusal(QStringLiteral("confirmation_required"),
                       QStringLiteral("Confirm the deletion with DELETE."));
    if (type == QStringLiteral("team.task.handover")) {
        const QString reason = payload.value(QStringLiteral("reason")).toString().trimmed();
        if (reason.isEmpty() || reason.size() > 500)
            return invalid(QStringLiteral("Give a reason of 1 to 500 characters."));
    }
    return std::nullopt;
}

// A failure as the phone receives it: its code, message and role, never local detail.
QJsonObject phoneFailure(const QJsonObject& local) {
    QJsonObject failure{
        {QStringLiteral("ok"), false},
        {QStringLiteral("error_code"), local.value(QStringLiteral("error_code"))},
        {QStringLiteral("error_message"), local.value(QStringLiteral("error_message"))}};
    if (local.contains(QStringLiteral("role")))
        failure.insert(QStringLiteral("role"), local.value(QStringLiteral("role")));
    return failure;
}

QJsonArray firstOf(const QJsonArray& values, const qsizetype count) {
    QJsonArray result;
    for (qsizetype index = 0; index < values.size() && index < count; ++index)
        result.append(values.at(index));
    return result;
}

} // namespace

bool SessionHost::isRemoteTeamCommand(const QString& type) {
    return allowedFields().contains(type);
}

bool SessionHost::isRemoteTeamRead(const QString& type) {
    return type == QStringLiteral("team.list") || type == QStringLiteral("team.get") ||
           type == QStringLiteral("team.task.get") || type == QStringLiteral("team.updates.list") ||
           type == QStringLiteral("team.decision.list");
}

QHash<QString, QJsonObject> SessionHost::teamMetadata() const {
    QHash<QString, QJsonObject> metadata;
    if (!teams_)
        return metadata;
    for (const teams::Team* team : teams_->teams())
        for (const teams::Member& member : team->members)
            metadata.insert(member.sessionId, {{QStringLiteral("id"), QString::number(team->id)},
                                               {QStringLiteral("revision"), team->revision},
                                               {QStringLiteral("role"), member.role}});
    return metadata;
}

void SessionHost::remoteTeamCommand(const QJsonObject& request, const teams::remote::Ids& ids,
                                    CommandCallback done) {
    const QString type = request.value(QStringLiteral("message_type")).toString();
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    if (!isRemoteTeamCommand(type)) {
        done(refusal(QStringLiteral("unknown_operation"),
                     QStringLiteral("The requested daemon operation is not supported.")));
        return;
    }
    if (const auto refused = payloadRefusal(type, payload)) {
        done(*refused);
        return;
    }
    // A change delivered again after its acknowledgement was lost answers as it did first.
    const QString messageId = request.value(QStringLiteral("message_id")).toString();
    if (isMutating(type)) {
        if (const QJsonObject cached = journal_->commandResult(messageId); !cached.isEmpty()) {
            done(cached);
            return;
        }
    }
    // What the command asks git is read off the event loop first, as for a local client.
    auto run = [this, guard = QPointer<SessionHost>(this), type, request, ids, messageId,
                done = std::move(done)] {
        prepareTeamTask(request, [this, guard, type, request, ids, messageId, done] {
            if (!guard)
                return;
            const QJsonObject result = remoteTeamAnswer(type, request, ids);
            if (isMutating(type))
                journal_->storeCommandResult(messageId, result);
            done(result);
        });
    };
    learnRepositories(teamCommandPaths(payload), std::move(run));
}

QJsonObject SessionHost::remoteTeamAnswer(const QString& type, const QJsonObject& request,
                                          const teams::remote::Ids& ids) {
    namespace remote = teams::remote;
    QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    const qint64 teamId = payload.value(QStringLiteral("team_id")).toString().toLongLong();
    const teams::Team* before = teams_ && teams_->isLoaded() ? teams_->team(teamId) : nullptr;
    if (type == QStringLiteral("team.get") && before &&
        payload.value(QStringLiteral("known_revision")).isDouble() &&
        payload.value(QStringLiteral("known_revision")).toInteger() == before->revision)
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("unchanged"), true},
                {QStringLiteral("revision"), before->revision}};
    if (type == QStringLiteral("team.decision.answer") && before &&
        payload.contains(QStringLiteral("choice"))) {
        const teams::Decision* decision =
            before->decision(payload.value(QStringLiteral("decision_id")).toString());
        if (decision &&
            !decision->choices.contains(payload.value(QStringLiteral("choice")).toString()))
            return invalid(QStringLiteral("Answer with one of the decision's choices."));
    }
    // The phone's page sizes, within the local protocol's.
    if (type == QStringLiteral("team.task.get") &&
        !payload.contains(QStringLiteral("history_limit")))
        payload.insert(QStringLiteral("history_limit"), remote::summaryUpdates);
    if ((type == QStringLiteral("team.updates.list") ||
         type == QStringLiteral("team.decision.list")) &&
        !payload.contains(QStringLiteral("limit")))
        payload.insert(QStringLiteral("limit"), remote::summaryUpdates);
    QJsonObject local = request;
    local.insert(QStringLiteral("payload"), payload);
    const QJsonObject answer = teamCommand(type, local);

    const teams::Team* team = teams_ ? teams_->team(teamId) : nullptr;
    const QJsonObject teamJson = team ? teams_->teamJson(*team) : QJsonObject{};
    QJsonObject result;
    if (!answer.value(QStringLiteral("ok")).toBool()) {
        result = phoneFailure(answer);
        // A stale_team refusal carries the team as it is, so the phone redraws it.
        if (answer.contains(QStringLiteral("team")))
            result.insert(QStringLiteral("team"),
                          remote::summary(answer.value(QStringLiteral("team")).toObject(), ids));
    } else if (type == QStringLiteral("team.list")) {
        const QJsonArray teams = answer.value(QStringLiteral("teams")).toArray();
        QJsonArray entries;
        for (const QJsonValue& value : teams)
            if (entries.size() < remote::maximumDirectoryEntries)
                entries.append(remote::directoryEntry(value.toObject(), ids));
        result = {{QStringLiteral("ok"), true},
                  {QStringLiteral("teams"), entries},
                  {QStringLiteral("teams_total"), teams.size()}};
    } else if (type == QStringLiteral("team.get")) {
        const QJsonArray updates = answer.value(QStringLiteral("updates")).toArray();
        result = {{QStringLiteral("ok"), true},
                  {QStringLiteral("team"), remote::summary(teamJson, ids)},
                  {QStringLiteral("updates"),
                   remote::updates(firstOf(updates, remote::summaryUpdates), teamJson)},
                  {QStringLiteral("updates_has_more"),
                   updates.size() > remote::summaryUpdates ||
                       answer.value(QStringLiteral("updates_has_more")).toBool()}};
    } else if (type == QStringLiteral("team.task.get")) {
        QJsonObject task = answer.value(QStringLiteral("task")).toObject();
        // The note of the newest change to the criteria, from the task's whole history.
        if (const teams::Task* stored =
                team ? team->task(payload.value(QStringLiteral("task_id")).toString()) : nullptr) {
            QString note;
            for (auto it = stored->history.crbegin(); it != stored->history.crend(); ++it)
                if (it->kind == QStringLiteral("criteria")) {
                    note = QStringLiteral("Changed %1: %2").arg(it->at, it->text).left(500);
                    break;
                }
            task.insert(QStringLiteral("criteria_note"), note);
        }
        result = {{QStringLiteral("ok"), true},
                  {QStringLiteral("task"), remote::taskDetail(task, teamJson, ids)},
                  {QStringLiteral("history_total"), task.value(QStringLiteral("history_total"))},
                  {QStringLiteral("history_start"), task.value(QStringLiteral("history_start"))}};
    } else if (type == QStringLiteral("team.updates.list")) {
        result = {{QStringLiteral("ok"), true},
                  {QStringLiteral("updates"),
                   remote::updates(answer.value(QStringLiteral("updates")).toArray(), teamJson)},
                  {QStringLiteral("has_more"), answer.value(QStringLiteral("has_more")).toBool()}};
    } else if (type == QStringLiteral("team.decision.list")) {
        result = {
            {QStringLiteral("ok"), true},
            {QStringLiteral("decisions"),
             remote::decisions(answer.value(QStringLiteral("decisions")).toArray(), teamJson)},
            {QStringLiteral("has_more"), answer.value(QStringLiteral("has_more")).toBool()}};
    } else if (type == QStringLiteral("team.delete")) {
        result = {{QStringLiteral("ok"), true},
                  {QStringLiteral("team_id"), answer.value(QStringLiteral("team_id"))}};
    } else {
        // A change: the team as it is after it.
        result = {{QStringLiteral("ok"), true},
                  {QStringLiteral("team"),
                   remote::summary(answer.value(QStringLiteral("team")).toObject(), ids)}};
    }
    if (const auto fits = remote::bounded(result))
        return *fits;
    return refusal(QStringLiteral("response_too_large"),
                   QStringLiteral("The team is too large to show on this device."));
}

} // namespace ketplus::daemon
