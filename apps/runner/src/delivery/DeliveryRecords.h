#pragma once

#include "delivery/DeliveryPorts.h"

#include <QJsonObject>
#include <QString>

#include <optional>

// The Relay's answers on the job routes, checked against the contract, and the journal record
// as JSON. A reply that breaks a rule reads as nothing.
namespace ketplus::runner::delivery {

// The success of job.acquire.
[[nodiscard]] std::optional<HeldLease> leaseFromReply(const QJsonObject& body);
// The `lease` of a runner_busy refusal: no requester, no runsOn.
[[nodiscard]] std::optional<HeldLease> busyLeaseFromRefusal(const QJsonObject& body);
// `startedAt` of job.start.
[[nodiscard]] std::optional<QString> startedAtFromReply(const QJsonObject& body);
// `job.reason` of a job_cancelled refusal; empty when it is null or unreadable.
[[nodiscard]] QString cancelReasonFromRefusal(const QJsonObject& body);

[[nodiscard]] bool isLeaseId(const QString& text);
[[nodiscard]] bool isRunnerLabel(const QString& text);

// {"version": 1, "step", ...}: see JournalRecord.
[[nodiscard]] QJsonObject journalToJson(const JournalRecord& record);
[[nodiscard]] std::optional<JournalRecord> journalFromJson(const QJsonObject& object);

} // namespace ketplus::runner::delivery
