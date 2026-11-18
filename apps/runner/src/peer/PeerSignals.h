#pragma once

#include "peer/PeerPorts.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QString>

#include <optional>

// The signed messages of one signaling attempt: the runner's offer and the desktop's answer.
// Pure functions over their inputs; the attempt's state lives in PeerConnector.
namespace ketplus::runner::peer {

inline constexpr qsizetype sdpLimit = 16 * 1024;
inline constexpr int candidateLimit = 64; // per side, plus the end-of-candidates marker
inline constexpr qsizetype candidateSizeLimit = 1024;
inline constexpr qsizetype candidateMidLimit = 64;
inline constexpr qint64 usedAnswerRetentionMs = 10 * 60 * 1000;
inline constexpr int usedAnswerLimit = 4096;

// What the offer names, besides its SDP.
struct OfferContext {
    QString connectionId;
    QString workspaceId; // of the desktop's pin
    QString machineId;
    QString runnerId;
    int runnerKeyVersion = 0;
    QString jobId;
    int attempt = 0;
};

// `signal.offer`, signed, or nothing when the SDP is too large or signing failed.
[[nodiscard]] std::optional<QJsonObject> signedOffer(const OfferContext& context,
                                                     const QString& sdp, const QDateTime& issuedAt,
                                                     const OfferSigner& signer);

// The single DTLS fingerprint an SDP names ("sha-256 AB:…"), normalized; empty when it names
// none, an invalid one, or two different ones.
[[nodiscard]] QString sdpFingerprint(const QString& sdp);

enum class AnswerError {
    Malformed,          // a field is missing, mistyped or out of bounds
    NotPinned,          // no active pin for the machine
    KeyVersionMismatch, // the answer names another key version than the pin
    IssuedAt,           // not RFC 3339 UTC, or more than five minutes from the clock
    InvalidSignature,
    Replayed, // this connection was already answered
    NoFingerprint,
};

[[nodiscard]] QString answerErrorCode(AnswerError error);

struct VerifiedAnswer {
    QString sdp;
    QString fingerprint;
};

// Connection ids whose answer was accepted, kept 10 minutes.
class UsedAnswers {
  public:
    [[nodiscard]] bool contains(const QString& connectionId, const QDateTime& now);
    void add(const QString& connectionId, const QDateTime& now);

  private:
    void expire(const QDateTime& now);
    QHash<QString, qint64> m_expiry;
};

struct AnswerCheck {
    std::optional<VerifiedAnswer> answer;
    AnswerError error = AnswerError::Malformed;
};

// Checks a `signal.answer` for the offer `context`/`offerSdp` against the machine's active pin.
// On success the connection id is recorded in `used`.
[[nodiscard]] AnswerCheck checkAnswer(const QJsonObject& message, const OfferContext& context,
                                      const QString& offerSdp,
                                      const std::optional<pairing::RunnerPin>& pin,
                                      const QDateTime& now, UsedAnswers& used);

} // namespace ketplus::runner::peer
