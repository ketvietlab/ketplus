#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace ketplus::runners {

// How a listed runner relates to this machine. Jobs go only to a trusted runner: this
// machine pinned its key and the relay pairs the two. Either flag alone is a mismatch, and so
// are both flags when the daemon's own `trusted` says false.
enum class Relation { Trusted, NotPaired, PinnedOnly, PairedOnly, Disputed };

// Why the page cannot show runners at all.
enum class Unavailable {
    None,
    NotEnrolled,       // no relay credential on this machine: sign in
    RelayUnavailable,  // the relay does not offer runners to this machine
    NoEntitlement,     // the account's plan has no runners
    DaemonUnsupported, // the daemon has no runner operations
    DaemonUnreachable, // no daemon answered
    Network,           // the relay could not be reached
};

struct RunnerRow {
    QString id;
    QString name;
    QString os;
    QString arch;
    QStringList labels;
    QString state; // as the relay reports it: idle, busy, draining, offline, disabled, ...
    QString version;
    std::optional<QDateTime> lastSeen;
    Relation relation = Relation::NotPaired;

    [[nodiscard]] bool trusted() const { return relation == Relation::Trusted; }
    [[nodiscard]] bool canPair() const { return relation != Relation::Trusted && !removed(); }
    [[nodiscard]] bool canDisable() const;
    [[nodiscard]] bool canEnable() const { return state == QStringLiteral("disabled"); }
    [[nodiscard]] bool canUnpair() const { return relation != Relation::NotPaired; }
    [[nodiscard]] bool canRemove() const { return !removed(); }
    [[nodiscard]] bool removed() const { return state == QStringLiteral("removed"); }
};

struct RunnerList {
    Unavailable unavailable = Unavailable::None;
    // A failure that is not one of the unavailable cases: shown as an error with a retry.
    QString errorCode;
    QString errorMessage;
    QList<RunnerRow> rows;
    // Runner ids this machine pinned that the relay no longer lists.
    QStringList unlistedPins;
    bool complete = true;
    // Some pins could not be read back; those runners are not trusted.
    bool pinError = false;

    [[nodiscard]] bool ok() const {
        return unavailable == Unavailable::None && errorCode.isEmpty();
    }
};

// Reads a runner.list answer. `error` is the transport error the requester reported (empty
// when the daemon answered). Fields that are missing or of the wrong type never make a runner
// trusted; a runner without a valid id is left out.
[[nodiscard]] RunnerList parseRunnerList(const QJsonObject& result, const QString& error);

// The unavailable case a failure code stands for, or None for an ordinary error.
[[nodiscard]] Unavailable unavailableFor(const QString& errorCode, const QString& error);

// After runner_unavailable: a remote.status answer that says this machine is not enrolled
// means signed out; anything else, a failed call included, keeps RelayUnavailable.
[[nodiscard]] Unavailable unavailableAfterStatus(const QJsonObject& status, const QString& error);

} // namespace ketplus::runners
