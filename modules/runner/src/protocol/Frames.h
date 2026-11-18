#pragma once

#include "protocol/ErrorCodes.h"
#include "protocol/FrameRoutes.h"

#include <QByteArray>
#include <QLatin1StringView>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>
#include <variant>

// The runner peer frames. Names of types and fields follow the protocol's frame table. Frames
// the desktop sends carry `epoch`; frames the runner sends do not (an `epoch` from the runner is
// ignored like any unknown field).
namespace ketplus::runner::protocol {

struct Hello {
    static constexpr QLatin1StringView type{"hello"};
    QString connectionId;
    QStringList capabilities; // must contain capability::runnerProtocol
    QString runnerId;
    QString runnerVersion;
    QString os;
    QString arch;
    QStringList labels;
    QString jobId;
    qint64 attempt = 1;
    QMap<QString, QString> snapshots; // workspaceKey -> snapshotId
    bool operator==(const Hello&) const = default;
};

struct Welcome {
    static constexpr QLatin1StringView type{"welcome"};
    QString connectionId;
    qint64 epoch = 0;
    QStringList capabilities;
    QString daemonVersion;
    bool operator==(const Welcome&) const = default;
};

// Exactly one of `argv` and `shell`.
struct Invocation {
    std::optional<QStringList> argv;
    std::optional<QString> shell;
    bool operator==(const Invocation&) const = default;
};

struct JobSpec {
    static constexpr QLatin1StringView type{"job.spec"};
    qint64 epoch = 0;
    QString jobId;
    qint64 attempt = 1;
    Invocation invocation;
    QString cwd;                               // relative to the worktree root; empty is the root
    QMap<QString, std::optional<QString>> env; // nullopt unsets the name
    qint64 timeoutMs = 0;
    QString repositoryKey;
    QString workspaceKey;
    QString snapshotId;
    QString sourceHead; // empty when the source worktree has no commit
    QString objectFormat;
    bool clean = false;
    bool operator==(const JobSpec&) const = default;
};

struct JobAccepted {
    static constexpr QLatin1StringView type{"job.accepted"};
    QString jobId;
    qint64 attempt = 1;
    bool operator==(const JobAccepted&) const = default;
};

struct JobRejected {
    static constexpr QLatin1StringView type{"job.rejected"};
    QString jobId;
    qint64 attempt = 1;
    QString code; // see errorCode()
    bool operator==(const JobRejected&) const = default;
};

struct JobResume {
    static constexpr QLatin1StringView type{"job.resume"};
    qint64 epoch = 0;
    QString jobId;
    qint64 outputOffset = 0;
    bool operator==(const JobResume&) const = default;
};

struct SnapshotOffer {
    static constexpr QLatin1StringView type{"snapshot.offer"};
    qint64 epoch = 0;
    QString jobId;
    QString snapshotId;
    std::optional<QString> baseId;
    qint64 packBytes = 0;
    QString packSha256;
    qint64 chunkBytes = 0;
    QStringList warnings; // known codes, no duplicates (decode drops repeats)
    bool operator==(const SnapshotOffer&) const = default;
};

struct SnapshotWant {
    static constexpr QLatin1StringView type{"snapshot.want"};
    QString snapshotId;
    QString state; // "have" or "need"
    qint64 fromOffset = 0;
    bool operator==(const SnapshotWant&) const = default;
};

struct SnapshotChunk {
    static constexpr QLatin1StringView type{"snapshot.chunk"};
    qint64 epoch = 0;
    QString snapshotId;
    qint64 offset = 0;
    qint64 bytes = 0;
    QString sha256;
    QString transferId;
    bool operator==(const SnapshotChunk&) const = default;
};

struct SnapshotAck {
    static constexpr QLatin1StringView type{"snapshot.ack"};
    QString snapshotId;
    qint64 offset = 0;
    bool operator==(const SnapshotAck&) const = default;
};

struct SnapshotReady {
    static constexpr QLatin1StringView type{"snapshot.ready"};
    QString snapshotId;
    bool operator==(const SnapshotReady&) const = default;
};

struct SnapshotError {
    static constexpr QLatin1StringView type{"snapshot.error"};
    QString snapshotId;
    QString code; // see errorCode()
    bool operator==(const SnapshotError&) const = default;
};

struct JobStarted {
    static constexpr QLatin1StringView type{"job.started"};
    QString jobId;
    QString startedAt;
    bool operator==(const JobStarted&) const = default;
};

struct JobOutput {
    static constexpr QLatin1StringView type{"job.output"};
    QString jobId;
    qint64 offset = 0;
    QByteArray data; // raw bytes; base64 on the wire
    bool operator==(const JobOutput&) const = default;
};

struct JobOutputGap {
    static constexpr QLatin1StringView type{"job.output.gap"};
    QString jobId;
    qint64 fromOffset = 0;
    qint64 toOffset = 0;
    bool operator==(const JobOutputGap&) const = default;
};

struct JobOutputAck {
    static constexpr QLatin1StringView type{"job.output.ack"};
    qint64 epoch = 0;
    QString jobId;
    qint64 offset = 0;
    bool operator==(const JobOutputAck&) const = default;
};

struct JobOutputResume {
    static constexpr QLatin1StringView type{"job.output.resume"};
    qint64 epoch = 0;
    QString jobId;
    qint64 fromOffset = 0;
    bool operator==(const JobOutputResume&) const = default;
};

struct JobCancel {
    static constexpr QLatin1StringView type{"job.cancel"};
    qint64 epoch = 0;
    QString jobId;
    QString reason; // "user", "deadline" or "session_closed"
    bool operator==(const JobCancel&) const = default;
};

struct JobResult {
    static constexpr QLatin1StringView type{"job.result"};
    QString jobId;
    qint64 attempt = 1;
    QString state;
    std::optional<qint64> exitCode;
    std::optional<qint64> signal;
    QString reason;
    QString cleanupStatus; // "verified" or "unverified"
    qint64 outputBytes = 0;
    std::optional<QString> startedAt;
    std::optional<QString> endedAt;
    QStringList warnings;
    bool operator==(const JobResult&) const = default;
};

struct JobResultAck {
    static constexpr QLatin1StringView type{"job.result.ack"};
    qint64 epoch = 0;
    QString jobId;
    bool operator==(const JobResultAck&) const = default;
};

// `ping`, `pong` and `close` go either way; `epoch` is set exactly when the desktop sends them.
struct Ping {
    static constexpr QLatin1StringView type{"ping"};
    std::optional<qint64> epoch;
    QString nonce;
    bool operator==(const Ping&) const = default;
};

struct Pong {
    static constexpr QLatin1StringView type{"pong"};
    std::optional<qint64> epoch;
    QString nonce;
    bool operator==(const Pong&) const = default;
};

struct Close {
    static constexpr QLatin1StringView type{"close"};
    std::optional<qint64> epoch;
    QString reason;
    bool operator==(const Close&) const = default;
};

using Frame = std::variant<Hello, Welcome, JobSpec, JobAccepted, JobRejected, JobResume,
                           SnapshotOffer, SnapshotWant, SnapshotChunk, SnapshotAck, SnapshotReady,
                           SnapshotError, JobStarted, JobOutput, JobOutputGap, JobOutputAck,
                           JobOutputResume, JobCancel, JobResult, JobResultAck, Ping, Pong, Close>;

[[nodiscard]] QLatin1StringView frameType(const Frame& frame);

// The known code of a `job.rejected` or `snapshot.error`; nullopt for a code from a newer peer.
[[nodiscard]] std::optional<ErrorCode> errorCode(const JobRejected& frame);
[[nodiscard]] std::optional<ErrorCode> errorCode(const SnapshotError& frame);

// Snapshot warning codes this version knows.
[[nodiscard]] const QStringList& knownWarnings();

// Why a frame was refused. `field` names the offending field (never its value); it is empty for
// errors about the message as a whole.
struct FrameError {
    enum class Kind {
        NotJson,           // not parseable JSON
        InvalidUtf8,       // the message is not valid UTF-8
        NotObject,         // JSON, but not an object
        MissingType,       // no string `type`
        UnsupportedType,   // a type this version does not know
        Oversize,          // above the frame's size limit: the caller closes the connection
        WrongChannel,      // a known type on a channel it does not use
        WrongDirection,    // a known type the sender may not send
        InvalidField,      // a field is missing, of the wrong JSON type or out of its limits
        MissingCapability, // `hello` without capability::runnerProtocol
    };
    Kind kind = Kind::InvalidField;
    QString type;
    QString field;
    // For an InvalidField `job.spec` whose own `jobId` and `attempt` are valid: those values, so
    // the receiver can answer `job.rejected` `spec_invalid`. Unset for every other error.
    std::optional<QString> jobId;
    std::optional<qint64> attempt;

    [[nodiscard]] static FrameError of(const Kind kind, const QString& type = {},
                                       const QString& field = {}) {
        FrameError error;
        error.kind = kind;
        error.type = type;
        error.field = field;
        return error;
    }
};

struct DecodeResult {
    std::optional<Frame> frame;
    FrameError error;
    [[nodiscard]] bool ok() const { return frame.has_value(); }
};

struct EncodeResult {
    QByteArray message;
    std::optional<FrameError> error;
    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

// Checks every field limit. Senders run it before sending (encode() does) and receivers on each
// decoded frame (decode() does), so both ends refuse the same frames.
[[nodiscard]] std::optional<FrameError> validate(const Frame& frame, Direction direction);

// One compact JSON message, or an error when the frame breaks a limit or its size.
[[nodiscard]] EncodeResult encode(const Frame& frame, Direction direction);

// Parses and validates one message received on `channel` from the sender `direction` names.
[[nodiscard]] DecodeResult decode(const QByteArray& message, Channel channel, Direction direction);

} // namespace ketplus::runner::protocol
