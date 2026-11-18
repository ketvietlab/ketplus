#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// The Control v2 runner answers a desktop reads, checked against the contract's schemas.
// Fields the schema does not name are ignored; a value that breaks the schema fails the answer.
namespace ketplus::daemon::runner {

struct RunnerTicket {
    QString ticketId;
    QString ticketSecret; // secret: only goes into the pairing token
    QDateTime expiresAt;
};

struct RunnerClaim {
    QString runnerId;
    QByteArray publicKey; // 32 raw bytes
    int keyVersion = 0;
    QString name;
    QString os;
    QString arch;
    QStringList labels;
    QString issuedAt;
    QByteArray signature; // 64 raw bytes
};

struct RunnerTicketStatus {
    QString ticketId;
    QString pairState;                // ticketed, claimed, active, expired, rejected, revoked
    std::optional<RunnerClaim> claim; // always set when claimed
};

struct RunnerSummary {
    QString runnerId;
    QString name;
    QString os;
    QString arch;
    QStringList labels;
    QString state; // offline, idle, busy, draining, disabled, removed
    QString runnerVersion;
    std::optional<QDateTime> lastSeenAt;
    bool pairedWithThisMachine = false;
};

struct RunnerPage {
    QList<RunnerSummary> runners;
    std::optional<QString> nextCursor;
};

[[nodiscard]] bool isRunnerId(const QString& value);
[[nodiscard]] std::optional<QDateTime> parseTime(const QString& value);

[[nodiscard]] std::optional<RunnerTicket> parseTicket(const QJsonValue& value);
[[nodiscard]] std::optional<RunnerTicketStatus> parseTicketStatus(const QJsonValue& value);
[[nodiscard]] std::optional<RunnerPage> parseRunnerPage(const QJsonValue& value);
// RunnerStateAnswer: the runner's state, when the answer names `runnerId`.
[[nodiscard]] std::optional<QString> parseStateAnswer(const QJsonValue& value,
                                                      const QString& runnerId);
// RunnerPairState: the pair state.
[[nodiscard]] std::optional<QString> parsePairState(const QJsonValue& value);

} // namespace ketplus::daemon::runner
