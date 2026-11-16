#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QStringList>
#include <optional>

namespace ketplus::daemon::teams::risk {

enum class Route { LeaderDirect, DeveloperLeader, DeveloperReviewer };
enum class Signal { Unknown, Clear, RequiresReview };

struct Check final {
    QString name;
    QString revision;
    QString evidenceId;
    bool passed{false};
};

// Supplied by the trusted host receipt adapter, never deserialized from a command.
// Missing facts remain unknown. Receipt producers must verify semantic risk coverage,
// not infer safety from a path or a submitter's severity/confidence alone.
struct VerifiedEvidence final {
    qint64 teamId{0};
    QString taskId;
    qint64 taskRevision{-1};
    QString id;
    QString revision;
    QMap<QString, Signal> domains;
    QStringList requiredChecks;
    QList<Check> checks;
    QString scopeEvidence;
    QString rollbackEvidence;
    bool materialRiskKnown{false};
    bool lowRiskVerified{false};
    bool userApprovalsSatisfied{false};
    int unresolvedFindings{-1};
    int unresolvedP0{-1};
    int unresolvedP1{-1};
    int unresolvedFailures{-1};
};

struct Evaluation final {
    Route route{Route::DeveloperReviewer};
    QString reason{QStringLiteral("evidence_missing")};
};

struct Decision final {
    qint64 teamId{0};
    QString taskId;
    Route route{Route::DeveloperReviewer};
    QString risk;
    QString kind;
    QString gateReason;
    int confidence{-1};
    QString reason;
    QString actor;
    QString revision;
    qint64 teamRevision{0};
    qint64 taskRevision{-1};
    QString at;
    QStringList evidenceIds;
    QString scopeEvidence;
    QString rollbackEvidence;
    QString followUp;
};

[[nodiscard]] QString routeName(Route route);
[[nodiscard]] bool advisoryKind(const QString& kind);
[[nodiscard]] QStringList mandatoryDomains();
[[nodiscard]] bool evidenceId(const QString& value);
[[nodiscard]] Evaluation evaluate(const VerifiedEvidence* evidence, int confidence);
[[nodiscard]] QJsonObject toJson(const Decision& decision);
// Legacy/malformed/unknown-version records never grant a route exception.
[[nodiscard]] std::optional<Decision> fromJson(const QJsonValue& value);

} // namespace ketplus::daemon::teams::risk
