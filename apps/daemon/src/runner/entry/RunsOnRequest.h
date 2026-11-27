#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace ketplus::daemon {
struct BackgroundTaskIntent;
}

// `runs_on` of a background task start: the labels that make the task a remote job.
namespace ketplus::daemon::runner::entry {

inline const QString runsOnKey = QStringLiteral("runs_on");

struct RunsOnRead {
    std::optional<QStringList> runsOn; // sorted without duplicates; nullopt without runs_on
    QString errorCode;
    QString error;
    [[nodiscard]] bool ok() const { return errorCode.isEmpty(); }
};

// The runs_on of a start payload. Refuses it on an agent task (runs_on_unsupported) and when it
// is not 1 to 16 valid labels (invalid_labels).
[[nodiscard]] RunsOnRead readRunsOn(const QJsonObject& payload);

// The payload with runs_on in its stored form, so the same labels in any order or repeated name
// the same request. A runs_on that does not read is left as it is.
[[nodiscard]] QJsonObject normalizedRequest(QJsonObject payload);

// The payload without runs_on, for the checks every task goes through.
[[nodiscard]] QJsonObject withoutRunsOn(QJsonObject payload);

// Fills the labels and timeout of a retry from its request payload, so a repeated start of a
// remote job compares with the stored one.
void prepareRetry(BackgroundTaskIntent& intent);

} // namespace ketplus::daemon::runner::entry
