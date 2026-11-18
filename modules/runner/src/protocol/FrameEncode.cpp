#include "protocol/FrameJson.h"
#include "protocol/Frames.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace ketplus::runner::protocol {
namespace {

using L = QLatin1StringView;

QJsonArray toArray(const QStringList& values) { return QJsonArray::fromStringList(values); }

QJsonValue nullable(const std::optional<QString>& value) {
    return value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null);
}

QJsonValue nullable(const std::optional<qint64>& value) {
    return value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null);
}

void write(QJsonObject& o, const Hello& f) {
    QJsonObject snapshots;
    for (auto it = f.snapshots.cbegin(); it != f.snapshots.cend(); ++it)
        snapshots.insert(it.key(), it.value());
    o.insert(L("connectionId"), f.connectionId);
    o.insert(L("capabilities"), toArray(f.capabilities));
    o.insert(L("runnerId"), f.runnerId);
    o.insert(L("runnerVersion"), f.runnerVersion);
    o.insert(L("os"), f.os);
    o.insert(L("arch"), f.arch);
    o.insert(L("labels"), toArray(f.labels));
    o.insert(L("jobId"), f.jobId);
    o.insert(L("attempt"), f.attempt);
    o.insert(L("snapshots"), snapshots);
}

void write(QJsonObject& o, const Welcome& f) {
    o.insert(L("connectionId"), f.connectionId);
    o.insert(L("epoch"), f.epoch);
    o.insert(L("capabilities"), toArray(f.capabilities));
    o.insert(L("daemonVersion"), f.daemonVersion);
}

void write(QJsonObject& o, const JobSpec& f) {
    QJsonObject invocation;
    if (f.invocation.argv)
        invocation.insert(L("argv"), toArray(*f.invocation.argv));
    if (f.invocation.shell)
        invocation.insert(L("shell"), *f.invocation.shell);
    QJsonObject env;
    for (auto it = f.env.cbegin(); it != f.env.cend(); ++it)
        env.insert(it.key(), nullable(it.value()));
    o.insert(L("epoch"), f.epoch);
    o.insert(L("jobId"), f.jobId);
    o.insert(L("attempt"), f.attempt);
    o.insert(L("invocation"), invocation);
    o.insert(L("cwd"), f.cwd);
    o.insert(L("env"), env);
    o.insert(L("timeoutMs"), f.timeoutMs);
    o.insert(L("repositoryKey"), f.repositoryKey);
    o.insert(L("workspaceKey"), f.workspaceKey);
    o.insert(L("snapshotId"), f.snapshotId);
    o.insert(L("sourceHead"), f.sourceHead);
    o.insert(L("objectFormat"), f.objectFormat);
    o.insert(L("clean"), f.clean);
}

void write(QJsonObject& o, const JobAccepted& f) {
    o.insert(L("jobId"), f.jobId);
    o.insert(L("attempt"), f.attempt);
}

void write(QJsonObject& o, const JobRejected& f) {
    o.insert(L("jobId"), f.jobId);
    o.insert(L("attempt"), f.attempt);
    o.insert(L("code"), f.code);
}

void write(QJsonObject& o, const JobResume& f) {
    o.insert(L("epoch"), f.epoch);
    o.insert(L("jobId"), f.jobId);
    o.insert(L("outputOffset"), f.outputOffset);
}

void write(QJsonObject& o, const SnapshotOffer& f) {
    o.insert(L("epoch"), f.epoch);
    o.insert(L("jobId"), f.jobId);
    o.insert(L("snapshotId"), f.snapshotId);
    o.insert(L("baseId"), nullable(f.baseId));
    o.insert(L("packBytes"), f.packBytes);
    o.insert(L("packSha256"), f.packSha256);
    o.insert(L("chunkBytes"), f.chunkBytes);
    o.insert(L("warnings"), toArray(f.warnings));
}

void write(QJsonObject& o, const SnapshotWant& f) {
    o.insert(L("snapshotId"), f.snapshotId);
    o.insert(L("state"), f.state);
    o.insert(L("fromOffset"), f.fromOffset);
}

void write(QJsonObject& o, const SnapshotChunk& f) {
    o.insert(L("epoch"), f.epoch);
    o.insert(L("snapshotId"), f.snapshotId);
    o.insert(L("offset"), f.offset);
    o.insert(L("bytes"), f.bytes);
    o.insert(L("sha256"), f.sha256);
    o.insert(L("transferId"), f.transferId);
}

void write(QJsonObject& o, const SnapshotAck& f) {
    o.insert(L("snapshotId"), f.snapshotId);
    o.insert(L("offset"), f.offset);
}

void write(QJsonObject& o, const SnapshotReady& f) { o.insert(L("snapshotId"), f.snapshotId); }

void write(QJsonObject& o, const SnapshotError& f) {
    o.insert(L("snapshotId"), f.snapshotId);
    o.insert(L("code"), f.code);
}

void write(QJsonObject& o, const JobStarted& f) {
    o.insert(L("jobId"), f.jobId);
    o.insert(L("startedAt"), f.startedAt);
}

void write(QJsonObject& o, const JobOutput& f) {
    o.insert(L("jobId"), f.jobId);
    o.insert(L("offset"), f.offset);
    o.insert(L("data"), QString::fromLatin1(f.data.toBase64()));
}

void write(QJsonObject& o, const JobOutputGap& f) {
    o.insert(L("jobId"), f.jobId);
    o.insert(L("fromOffset"), f.fromOffset);
    o.insert(L("toOffset"), f.toOffset);
}

void write(QJsonObject& o, const JobOutputAck& f) {
    o.insert(L("epoch"), f.epoch);
    o.insert(L("jobId"), f.jobId);
    o.insert(L("offset"), f.offset);
}

void write(QJsonObject& o, const JobOutputResume& f) {
    o.insert(L("epoch"), f.epoch);
    o.insert(L("jobId"), f.jobId);
    o.insert(L("fromOffset"), f.fromOffset);
}

void write(QJsonObject& o, const JobCancel& f) {
    o.insert(L("epoch"), f.epoch);
    o.insert(L("jobId"), f.jobId);
    o.insert(L("reason"), f.reason);
}

void write(QJsonObject& o, const JobResult& f) {
    o.insert(L("jobId"), f.jobId);
    o.insert(L("attempt"), f.attempt);
    o.insert(L("state"), f.state);
    o.insert(L("exitCode"), nullable(f.exitCode));
    o.insert(L("signal"), nullable(f.signal));
    o.insert(L("reason"), f.reason);
    o.insert(L("cleanupStatus"), f.cleanupStatus);
    o.insert(L("outputBytes"), f.outputBytes);
    o.insert(L("startedAt"), nullable(f.startedAt));
    o.insert(L("endedAt"), nullable(f.endedAt));
    o.insert(L("warnings"), toArray(f.warnings));
}

void write(QJsonObject& o, const JobResultAck& f) {
    o.insert(L("epoch"), f.epoch);
    o.insert(L("jobId"), f.jobId);
}

void writeEpoch(QJsonObject& o, const std::optional<qint64>& epoch) {
    if (epoch)
        o.insert(L("epoch"), *epoch);
}

void write(QJsonObject& o, const Ping& f) {
    writeEpoch(o, f.epoch);
    o.insert(L("nonce"), f.nonce);
}

void write(QJsonObject& o, const Pong& f) {
    writeEpoch(o, f.epoch);
    o.insert(L("nonce"), f.nonce);
}

void write(QJsonObject& o, const Close& f) {
    writeEpoch(o, f.epoch);
    o.insert(L("reason"), f.reason);
}

} // namespace

QJsonObject detail::toObject(const Frame& frame) {
    QJsonObject object;
    object.insert(L("type"), frameType(frame));
    std::visit([&object](const auto& f) { write(object, f); }, frame);
    return object;
}

EncodeResult encode(const Frame& frame, const Direction direction) {
    const QLatin1StringView type = frameType(frame);
    if (std::optional<FrameError> error = validate(frame, direction))
        return {{}, std::move(error)};
    QByteArray message = QJsonDocument(detail::toObject(frame)).toJson(QJsonDocument::Compact);
    if (message.size() > routeFor(type)->maxBytes)
        return {{}, FrameError::of(FrameError::Kind::Oversize, type)};
    return {std::move(message), std::nullopt};
}

} // namespace ketplus::runner::protocol
