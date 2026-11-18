#pragma once

#include <QDateTime>
#include <QJsonValue>
#include <QString>

#include <optional>

// The Relay's view of a remote job (RunnerJob), checked against the contract's schema. Fields
// the schema does not name are ignored; a value that breaks the schema fails the answer.
namespace ketplus::daemon::runner::jobs {

struct RelayJob {
    QString jobId;
    QString state; // queued, offered, acquired, started, finished, abandoned, expired,
                   // cancelled, failed
    int attempt = 0;
    std::optional<QString> runnerId;
    QDateTime queuedAt;
    QDateTime expiresAt;
    std::optional<QDateTime> startedAt;
    std::optional<QDateTime> endedAt;
    std::optional<QString> outcome; // succeeded, failed, cancelled, interrupted, rejected
    std::optional<QString> reason;

    [[nodiscard]] bool terminal() const;
};

[[nodiscard]] bool isJobId(const QString& value);
[[nodiscard]] bool isSubmitKey(const QString& value);
[[nodiscard]] bool isReasonCode(const QString& value);
[[nodiscard]] std::optional<RelayJob> parseRelayJob(const QJsonValue& value);

} // namespace ketplus::daemon::runner::jobs
