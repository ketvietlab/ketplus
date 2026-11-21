#pragma once

#include "teams/TeamRiskPolicy.h"
#include <QJsonObject>
#include <QList>
#include <QString>
#include <optional>

namespace ketplus::daemon::teams::review_policy {

// Version 1: evidence and closure conditions are references/codes, never raw output.
struct Finding final {
    QString id;
    QString severity{QStringLiteral("unknown")};
    QString impact{QStringLiteral("unknown")};
    QString scope{QStringLiteral("unknown")};
    QString evidence;
    QString closeWhen;
    bool mandatoryRule{true};
    QString status{QStringLiteral("open")};
    QString actor;
    QString reason;
    QString owner;
    QString resolvedRevision;
    QString receipt;
    QString at;
};

struct Record final {
    QString revision;
    QString reviewer;
    int round{0};
    bool legacy{false};
    QList<Finding> findings;
};

struct Resolution final {
    QString id;
    QString status;
    QString reason;
    QString owner;
};
struct Request final {
    QString action;
    qint64 teamRevision{-1};
    qint64 taskRevision{-1};
    QString head;
    QString receipt;
    int confidence{-1};
    QList<Resolution> resolutions;
};
[[nodiscard]] std::optional<Request> request(const QString& json);
// Host attestation is independent of the command; the coordinator also fences live state.
[[nodiscard]] std::optional<Record> resolve(const Record& record, const Request& request,
                                            const risk::VerifiedEvidence& receipt,
                                            const QString& actor, const QString& at);
[[nodiscard]] bool code(const QString& value);
[[nodiscard]] bool revision(const QString& value);
[[nodiscard]] bool blocks(const Finding& finding);
[[nodiscard]] bool blocks(const Record& record);
[[nodiscard]] bool deferrable(const Finding& finding);
[[nodiscard]] bool leaderVerifiable(const Finding& finding);
[[nodiscard]] QJsonObject toJson(const Record& record);
[[nodiscard]] std::optional<Record> fromJson(const QJsonValue& value);
// Input has no resolution fields. The daemon supplies reviewer, round and revision.
[[nodiscard]] std::optional<QList<Finding>> report(const QJsonObject& value);
// Missing/malformed structured findings remain a blocking legacy record.
[[nodiscard]] std::optional<Record> reply(const QString& text, const QString& revision,
                                          const QString& reviewer, int round, bool findingsVerdict);

} // namespace ketplus::daemon::teams::review_policy
