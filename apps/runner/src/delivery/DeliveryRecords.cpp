#include "delivery/DeliveryRecords.h"

#include "protocol/FrameRules.h"

#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>

namespace ketplus::runner::delivery {

namespace {

constexpr int journalVersion = 1;

bool matches(const QRegularExpression& pattern, const QString& text) {
    return pattern.match(text).hasMatch();
}

bool isId(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    return matches(pattern, text);
}

bool isReason(const QString& text) { return protocol::rules::isReasonCode(text); }

bool isTime(const QString& text) {
    return !text.isEmpty() && QDateTime::fromString(text, Qt::ISODateWithMs).isValid();
}

std::optional<qint64> attemptOf(const QJsonValue& value) {
    if (!value.isDouble())
        return std::nullopt;
    const double number = value.toDouble();
    if (number < 1 || number > 3 || number != qint64(number))
        return std::nullopt;
    return qint64(number);
}

std::optional<QStringList> runsOnOf(const QJsonValue& value) {
    if (!value.isArray())
        return std::nullopt;
    const QJsonArray array = value.toArray();
    if (array.isEmpty() || array.size() > 16)
        return std::nullopt;
    QStringList labels;
    for (const QJsonValue& label : array) {
        if (!label.isString() || !isRunnerLabel(label.toString()) ||
            labels.contains(label.toString()))
            return std::nullopt;
        labels.append(label.toString());
    }
    return labels;
}

const QStringList stepNames{QStringLiteral("acquiring"), QStringLiteral("leased"),
                            QStringLiteral("spec_accepted"), QStringLiteral("started"),
                            QStringLiteral("closing")};

const QStringList finishOutcomes{QStringLiteral("succeeded"), QStringLiteral("failed"),
                                 QStringLiteral("cancelled"), QStringLiteral("interrupted"),
                                 QStringLiteral("rejected")};

const QStringList releaseReasons{
    QStringLiteral("peer_unreachable"),   QStringLiteral("untrusted_requester"),
    QStringLiteral("label_mismatch"),     QStringLiteral("draining"),
    QStringLiteral("already_dispatched"), QStringLiteral("snapshot_unavailable")};

} // namespace

bool isLeaseId(const QString& text) { return isId(text); }

bool isRunnerLabel(const QString& text) {
    static const QRegularExpression pattern(
        QStringLiteral("^(os:linux|arch:(x86_64|arm64)|[a-z0-9][a-z0-9._-]*)$"));
    return text.size() <= 64 && matches(pattern, text);
}

std::optional<HeldLease> leaseFromReply(const QJsonObject& body) {
    HeldLease lease;
    lease.jobId = body.value(QStringLiteral("jobId")).toString();
    lease.leaseId = body.value(QStringLiteral("leaseId")).toString();
    lease.leaseExpiresAt = body.value(QStringLiteral("leaseExpiresAt")).toString();
    const QJsonObject requester = body.value(QStringLiteral("requester")).toObject();
    lease.machineId = requester.value(QStringLiteral("machineId")).toString();
    lease.workspaceId = requester.value(QStringLiteral("workspaceId")).toString();
    const std::optional<qint64> attempt = attemptOf(body.value(QStringLiteral("attempt")));
    const std::optional<QStringList> runsOn = runsOnOf(body.value(QStringLiteral("runsOn")));
    if (!isId(lease.jobId) || !isId(lease.leaseId) || !isTime(lease.leaseExpiresAt) ||
        !isId(lease.machineId) || !isId(lease.workspaceId) || !attempt || !runsOn)
        return std::nullopt;
    lease.attempt = *attempt;
    lease.runsOn = *runsOn;
    return lease;
}

std::optional<HeldLease> busyLeaseFromRefusal(const QJsonObject& body) {
    const QJsonObject held = body.value(QStringLiteral("lease")).toObject();
    HeldLease lease;
    lease.jobId = held.value(QStringLiteral("jobId")).toString();
    lease.leaseId = held.value(QStringLiteral("leaseId")).toString();
    lease.leaseExpiresAt = held.value(QStringLiteral("leaseExpiresAt")).toString();
    const std::optional<qint64> attempt = attemptOf(held.value(QStringLiteral("attempt")));
    if (!isId(lease.jobId) || !isId(lease.leaseId) || !isTime(lease.leaseExpiresAt) || !attempt)
        return std::nullopt;
    lease.attempt = *attempt;
    return lease;
}

std::optional<QString> startedAtFromReply(const QJsonObject& body) {
    const QString startedAt = body.value(QStringLiteral("startedAt")).toString();
    return isTime(startedAt) ? std::optional<QString>(startedAt) : std::nullopt;
}

QString cancelReasonFromRefusal(const QJsonObject& body) {
    const QString reason =
        body.value(QStringLiteral("job")).toObject().value(QStringLiteral("reason")).toString();
    return isReason(reason) ? reason : QString();
}

QJsonObject journalToJson(const JournalRecord& record) {
    QJsonObject object{{QStringLiteral("version"), journalVersion},
                       {QStringLiteral("step"), stepNames.at(int(record.step))}};
    if (record.step == JournalRecord::Step::Acquiring) {
        object.insert(QStringLiteral("acquireId"), record.acquireId);
        if (!record.wantedJobId.isEmpty())
            object.insert(QStringLiteral("jobId"), record.wantedJobId);
        return object;
    }
    const HeldLease& lease = record.lease;
    object.insert(QStringLiteral("jobId"), lease.jobId);
    object.insert(QStringLiteral("attempt"), lease.attempt);
    object.insert(QStringLiteral("leaseId"), lease.leaseId);
    object.insert(QStringLiteral("leaseExpiresAt"), lease.leaseExpiresAt);
    // Empty for a lease learned from runner_busy.
    object.insert(QStringLiteral("machineId"), lease.machineId);
    object.insert(QStringLiteral("workspaceId"), lease.workspaceId);
    object.insert(QStringLiteral("runsOn"), QJsonArray::fromStringList(lease.runsOn));
    if (!record.startedAt.isEmpty())
        object.insert(QStringLiteral("startedAt"), record.startedAt);
    if (record.step == JournalRecord::Step::Closing) {
        object.insert(QStringLiteral("close"),
                      record.release ? QStringLiteral("release") : QStringLiteral("finish"));
        if (!record.release)
            object.insert(QStringLiteral("outcome"), record.outcome);
        object.insert(QStringLiteral("reason"), record.reason);
    }
    return object;
}

std::optional<JournalRecord> journalFromJson(const QJsonObject& object) {
    if (object.value(QStringLiteral("version")).toInteger(-1) != journalVersion)
        return std::nullopt;
    const qsizetype step = stepNames.indexOf(object.value(QStringLiteral("step")).toString());
    if (step < 0)
        return std::nullopt;
    JournalRecord record;
    record.step = JournalRecord::Step(step);
    if (record.step == JournalRecord::Step::Acquiring) {
        record.acquireId = object.value(QStringLiteral("acquireId")).toString();
        record.wantedJobId = object.value(QStringLiteral("jobId")).toString();
        if (!isId(record.acquireId) || (!record.wantedJobId.isEmpty() && !isId(record.wantedJobId)))
            return std::nullopt;
        return record;
    }
    HeldLease& lease = record.lease;
    lease.jobId = object.value(QStringLiteral("jobId")).toString();
    lease.leaseId = object.value(QStringLiteral("leaseId")).toString();
    lease.leaseExpiresAt = object.value(QStringLiteral("leaseExpiresAt")).toString();
    lease.machineId = object.value(QStringLiteral("machineId")).toString();
    lease.workspaceId = object.value(QStringLiteral("workspaceId")).toString();
    const std::optional<qint64> attempt = attemptOf(object.value(QStringLiteral("attempt")));
    const QJsonValue runsOnValue = object.value(QStringLiteral("runsOn"));
    const std::optional<QStringList> runsOn = runsOnValue.toArray().isEmpty()
                                                  ? std::optional<QStringList>(QStringList{})
                                                  : runsOnOf(runsOnValue);
    if (!isId(lease.jobId) || !isId(lease.leaseId) || !isTime(lease.leaseExpiresAt) || !attempt ||
        !runsOnValue.isArray() || !runsOn ||
        (!lease.machineId.isEmpty() && !isId(lease.machineId)) ||
        (!lease.workspaceId.isEmpty() && !isId(lease.workspaceId)))
        return std::nullopt;
    lease.attempt = *attempt;
    lease.runsOn = *runsOn;
    record.startedAt = object.value(QStringLiteral("startedAt")).toString();
    if (!record.startedAt.isEmpty() && !isTime(record.startedAt))
        return std::nullopt;
    if (record.step == JournalRecord::Step::Closing) {
        const QString close = object.value(QStringLiteral("close")).toString();
        record.release = close == QStringLiteral("release");
        record.outcome = object.value(QStringLiteral("outcome")).toString();
        record.reason = object.value(QStringLiteral("reason")).toString();
        const bool valid = record.release ? releaseReasons.contains(record.reason)
                                          : close == QStringLiteral("finish") &&
                                                finishOutcomes.contains(record.outcome) &&
                                                isReason(record.reason);
        if (!valid)
            return std::nullopt;
    }
    return record;
}

RestartPolicy::Finish NeverRelaunch::afterRestart(const JournalRecord& record) const {
    if (record.step == JournalRecord::Step::Closing)
        return {record.release, record.outcome, record.reason};
    return {false, QStringLiteral("interrupted"), QStringLiteral("runner_restarted")};
}

} // namespace ketplus::runner::delivery
