#include "runner/jobs/RemoteJobSchema.h"

#include "runner/RunnerRelaySchema.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <cmath>

namespace ketplus::daemon::runner::jobs {
namespace {

const QSet<QString>& jobStates() {
    static const QSet<QString> states{
        QStringLiteral("queued"),  QStringLiteral("offered"),   QStringLiteral("acquired"),
        QStringLiteral("started"), QStringLiteral("finished"),  QStringLiteral("abandoned"),
        QStringLiteral("expired"), QStringLiteral("cancelled"), QStringLiteral("failed")};
    return states;
}

const QSet<QString>& outcomes() {
    static const QSet<QString> values{QStringLiteral("succeeded"), QStringLiteral("failed"),
                                      QStringLiteral("cancelled"), QStringLiteral("interrupted"),
                                      QStringLiteral("rejected")};
    return values;
}

// A required member that is a date-time, or null when `nullable`.
bool readTime(const QJsonObject& object, const char* key, const bool nullable,
              std::optional<QDateTime>& out) {
    const QJsonValue value = object.value(QLatin1StringView(key));
    if (value.isNull() && nullable && object.contains(QLatin1StringView(key)))
        return true;
    if (!value.isString())
        return false;
    out = parseTime(value.toString());
    return out.has_value();
}

} // namespace

bool RelayJob::terminal() const {
    return state == QStringLiteral("finished") || state == QStringLiteral("abandoned") ||
           state == QStringLiteral("expired") || state == QStringLiteral("cancelled") ||
           state == QStringLiteral("failed");
}

bool isJobId(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    return pattern.match(value).hasMatch();
}

bool isSubmitKey(const QString& value) { return isJobId(value); }

bool isReasonCode(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z][a-z0-9_]{0,63}$"));
    return pattern.match(value).hasMatch();
}

std::optional<RelayJob> parseRelayJob(const QJsonValue& value) {
    if (!value.isObject())
        return std::nullopt;
    const QJsonObject object = value.toObject();
    RelayJob job;
    job.jobId = object.value(QLatin1StringView("jobId")).toString();
    job.state = object.value(QLatin1StringView("state")).toString();
    if (!isJobId(job.jobId) || !jobStates().contains(job.state))
        return std::nullopt;

    const QJsonValue attempt = object.value(QLatin1StringView("attempt"));
    const double number = attempt.toDouble(-1);
    if (!attempt.isDouble() || number != std::floor(number) || number < 0 || number > 3)
        return std::nullopt;
    job.attempt = static_cast<int>(number);

    const QJsonValue runner = object.value(QLatin1StringView("runnerId"));
    if (runner.isString() && isRunnerId(runner.toString()))
        job.runnerId = runner.toString();
    else if (!runner.isNull() || !object.contains(QLatin1StringView("runnerId")))
        return std::nullopt;

    std::optional<QDateTime> queuedAt;
    std::optional<QDateTime> expiresAt;
    if (!readTime(object, "queuedAt", false, queuedAt) ||
        !readTime(object, "expiresAt", false, expiresAt) ||
        !readTime(object, "startedAt", true, job.startedAt) ||
        !readTime(object, "endedAt", true, job.endedAt))
        return std::nullopt;
    job.queuedAt = *queuedAt;
    job.expiresAt = *expiresAt;

    const QJsonValue outcome = object.value(QLatin1StringView("outcome"));
    if (outcome.isString() && outcomes().contains(outcome.toString()))
        job.outcome = outcome.toString();
    else if (!outcome.isNull() || !object.contains(QLatin1StringView("outcome")))
        return std::nullopt;

    const QJsonValue reason = object.value(QLatin1StringView("reason"));
    if (reason.isString() && isReasonCode(reason.toString()))
        job.reason = reason.toString();
    else if (!reason.isNull() || !object.contains(QLatin1StringView("reason")))
        return std::nullopt;
    return job;
}

} // namespace ketplus::daemon::runner::jobs
