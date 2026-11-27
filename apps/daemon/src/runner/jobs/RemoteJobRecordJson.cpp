#include "runner/jobs/RemoteJobRecordJson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimeZone>

#include <climits>

namespace ketplus::daemon::runner::jobs {

namespace {

constexpr int version = 1;

const QSet<QString>& states() {
    static const QSet<QString> all{state::queued,    state::starting,  state::running,
                                   state::stopping,  state::completed, state::failed,
                                   state::cancelled, state::timedOut,  state::interrupted};
    return all;
}
const QSet<QString>& placements() {
    static const QSet<QString> all{placement::submitting, placement::relayQueued,
                                   placement::assigned, placement::syncing, placement::running};
    return all;
}
const QSet<QString>& cleanups() {
    static const QSet<QString> all{cleanup::notRequired, cleanup::pending, cleanup::verified,
                                   cleanup::unverified};
    return all;
}

void putTime(QJsonObject& object, const char* name, const std::optional<QDateTime>& value) {
    if (value)
        object.insert(QLatin1StringView(name), value->toMSecsSinceEpoch());
}
void putNumber(QJsonObject& object, const char* name, const std::optional<qint64>& value) {
    if (value)
        object.insert(QLatin1StringView(name), *value);
}

// A field that is absent reads as unset; one of another type fails the record.
bool readString(const QJsonObject& object, const char* name, QString& out) {
    const QJsonValue value = object.value(QLatin1StringView(name));
    if (value.isUndefined())
        return true;
    if (!value.isString())
        return false;
    out = value.toString();
    return true;
}
bool readNumber(const QJsonObject& object, const char* name, std::optional<qint64>& out) {
    const QJsonValue value = object.value(QLatin1StringView(name));
    if (value.isUndefined())
        return true;
    if (!value.isDouble() || value.toDouble() != double(value.toInteger()))
        return false;
    out = value.toInteger();
    return true;
}
bool readTime(const QJsonObject& object, const char* name, std::optional<QDateTime>& out) {
    std::optional<qint64> msecs;
    if (!readNumber(object, name, msecs))
        return false;
    if (msecs)
        out = QDateTime::fromMSecsSinceEpoch(*msecs, QTimeZone::UTC);
    return true;
}

} // namespace

QByteArray encodeRecord(const RemoteJobRecord& record) {
    QJsonObject object{{"version", version},
                       {"taskId", record.taskId},
                       {"runsOn", QJsonArray::fromStringList(record.runsOn)},
                       {"timeoutMs", record.timeoutMs},
                       {"state", record.state},
                       {"placement", record.placement},
                       {"reason", record.reason},
                       {"cleanupStatus", record.cleanupStatus},
                       {"jobId", record.jobId},
                       {"stopReason", record.stopReason},
                       {"stopCode", record.stopCode},
                       {"dispatchedTo", record.dispatchedTo}};
    if (record.runner)
        object.insert("runner", QJsonObject{{"runnerId", record.runner->runnerId},
                                            {"name", record.runner->name},
                                            {"attempt", record.runner->attempt}});
    putTime(object, "dispatchedAt", record.dispatchedAt);
    putTime(object, "lastPeerContact", record.lastPeerContact);
    putTime(object, "startedAt", record.startedAt);
    putNumber(object, "exitCode", record.exitCode);
    putNumber(object, "signal", record.signal);
    if (record.snapshot)
        object.insert(
            "snapshot",
            QJsonObject{{"repositoryId", record.snapshot->repositoryId},
                        {"worktreeId", record.snapshot->worktreeId},
                        {"snapshotId", record.snapshot->snapshotId},
                        {"warnings", QJsonArray::fromStringList(record.snapshot->warnings)}});
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::optional<RemoteJobRecord> decodeRecord(const QByteArray& bytes) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    const QJsonObject object = document.object();
    if (object.value("version").toInt(-1) != version)
        return std::nullopt;
    RemoteJobRecord record;
    std::optional<qint64> timeout;
    bool ok = readString(object, "taskId", record.taskId) &&
              readNumber(object, "timeoutMs", timeout) &&
              readString(object, "state", record.state) &&
              readString(object, "placement", record.placement) &&
              readString(object, "reason", record.reason) &&
              readString(object, "cleanupStatus", record.cleanupStatus) &&
              readString(object, "jobId", record.jobId) &&
              readString(object, "stopReason", record.stopReason) &&
              readString(object, "stopCode", record.stopCode) &&
              readString(object, "dispatchedTo", record.dispatchedTo) &&
              readTime(object, "dispatchedAt", record.dispatchedAt) &&
              readTime(object, "lastPeerContact", record.lastPeerContact) &&
              readTime(object, "startedAt", record.startedAt) &&
              readNumber(object, "exitCode", record.exitCode) &&
              readNumber(object, "signal", record.signal);
    if (!ok || record.taskId.isEmpty() || !timeout || *timeout < 0 ||
        !states().contains(record.state) || !placements().contains(record.placement) ||
        !cleanups().contains(record.cleanupStatus))
        return std::nullopt;
    record.timeoutMs = *timeout;
    const QJsonValue labels = object.value("runsOn");
    if (!labels.isArray())
        return std::nullopt;
    for (const QJsonValue& label : labels.toArray()) {
        if (!label.isString())
            return std::nullopt;
        record.runsOn.append(label.toString());
    }
    const QJsonValue runner = object.value("runner");
    if (!runner.isUndefined()) {
        const QJsonObject held = runner.toObject();
        std::optional<qint64> attempt;
        RemoteRunner value;
        if (!runner.isObject() || !readString(held, "runnerId", value.runnerId) ||
            !readString(held, "name", value.name) || !readNumber(held, "attempt", attempt) ||
            value.runnerId.isEmpty() || !attempt || *attempt < 0 || *attempt > INT_MAX)
            return std::nullopt;
        value.attempt = int(*attempt);
        record.runner = value;
    }
    const QJsonValue snapshot = object.value("snapshot");
    if (!snapshot.isUndefined()) {
        const QJsonObject held = snapshot.toObject();
        RemoteSnapshot value;
        if (!snapshot.isObject() || !readString(held, "repositoryId", value.repositoryId) ||
            !readString(held, "worktreeId", value.worktreeId) ||
            !readString(held, "snapshotId", value.snapshotId) || value.repositoryId.isEmpty() ||
            value.worktreeId.isEmpty() || value.snapshotId.isEmpty() ||
            !held.value("warnings").isArray())
            return std::nullopt;
        for (const QJsonValue& warning : held.value("warnings").toArray()) {
            if (!warning.isString())
                return std::nullopt;
            value.warnings.append(warning.toString());
        }
        record.snapshot = value;
    }
    return record;
}

} // namespace ketplus::daemon::runner::jobs
