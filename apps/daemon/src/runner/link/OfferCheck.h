#pragma once

#include "runner/link/LinkPorts.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <optional>

// The signed messages of the desktop's side of one runner signaling attempt: the runner's offer
// it checks and the answer it signs. Pure functions over their inputs.
namespace ketplus::daemon::runner::link {

inline constexpr qsizetype sdpLimit = 16 * 1024;
inline constexpr int candidateLimit = 64; // per side, plus the end-of-candidates marker
inline constexpr qsizetype candidateSizeLimit = 1024;
inline constexpr qsizetype candidateMidLimit = 64;
inline constexpr qsizetype idLimit = 128;
// What the Relay issues for every attempt: `pc_` and 24 base64url characters.
inline constexpr char connectionIdPattern[] = "^pc_[A-Za-z0-9_-]{24}$";
inline constexpr qint64 usedOfferRetentionMs = 10 * 60 * 1000;
inline constexpr int usedOfferLimit = 4096;

// In the order the checks run.
enum class OfferError {
    Unreadable,         // no usable connection id: dropped without an answer
    Replayed,           // the connection id was used within ten minutes: dropped
    Malformed,          // a field is missing, mistyped or out of bounds
    NotPinned,          // no pin for the runner
    KeyVersionMismatch, // the offer names another key version than the pin
    InvalidSignature,
    Expired,  // issuedAt more than five minutes from the clock
    UsedFull, // the table of used connection ids is full
};

// The `signal.end` reason for a refused offer; empty for the errors dropped without one.
[[nodiscard]] QString offerErrorReason(OfferError error);

// What a verified offer names. Every field is covered by the runner's signature, except
// `iceServers`, which the Relay adds for this machine.
struct VerifiedOffer {
    QString connectionId;
    QString runnerId;
    int runnerKeyVersion = 0;
    QString jobId;
    int attempt = 0;
    QString sdp;
    QJsonArray iceServers;
};

// Connection ids of offers whose signature verified, kept ten minutes.
class UsedOffers {
  public:
    [[nodiscard]] bool contains(const QString& connectionId, const QDateTime& now);
    // False when the table is full.
    bool add(const QString& connectionId, const QDateTime& now);

  private:
    void expire(const QDateTime& now);
    QHash<QString, qint64> m_expiry;
};

struct OfferCheck {
    std::optional<VerifiedOffer> offer;
    OfferError error = OfferError::Unreadable;
    QString connectionId; // set once readable
};

// True for a `signal.offer` of a runner: string `runnerId` and `jobId`, and neither `grant` nor
// `deviceId`. Not a security boundary: each path checks its own signature.
[[nodiscard]] bool isRunnerOffer(const QJsonObject& message);

// Checks a runner's `signal.offer` addressed to `identity` against the pin of the runner the
// Relay names. The connection id is recorded in `used` only once the signature verified, so an
// unverified offer never takes a place there.
[[nodiscard]] OfferCheck checkOffer(const QJsonObject& message, const MachineIdentity& identity,
                                    const RunnerPins& pins, const QDateTime& now, UsedOffers& used);

// `signal.answer` for `offer`, signed by the machine key, or nothing when the SDP is too large
// or signing failed.
[[nodiscard]] std::optional<QJsonObject> signedAnswer(const VerifiedOffer& offer,
                                                      const QString& sdp, const QDateTime& issuedAt,
                                                      const MachineIdentity& identity);

} // namespace ketplus::daemon::runner::link
