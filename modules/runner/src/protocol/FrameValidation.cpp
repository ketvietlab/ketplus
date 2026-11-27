#include "protocol/Capabilities.h"
#include "protocol/FrameLimits.h"
#include "protocol/FrameRules.h"
#include "protocol/Frames.h"

#include <QDate>
#include <QRegularExpression>
#include <QSet>
#include <QTime>

#include <type_traits>

namespace ketplus::runner::protocol {
namespace {

bool isLowerHex(const QString& text, const qsizetype length) {
    if (text.size() != length)
        return false;
    for (const QChar c : text) {
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f')))
            return false;
    }
    return true;
}

bool isObjectId(const QString& text) { return isLowerHex(text, 40) || isLowerHex(text, 64); }

// The object id length of a repository's object format.
qsizetype objectIdLength(const QString& objectFormat) {
    return objectFormat == QLatin1StringView("sha256") ? 64 : 40;
}

bool isPrintableAscii(const QString& text, const qsizetype maxChars) {
    if (text.size() > maxChars)
        return false;
    for (const QChar c : text) {
        if (c.unicode() < 0x20 || c.unicode() > 0x7e)
            return false;
    }
    return true;
}

// Workspace and repository keys: the first 32 characters of a base64url digest.
bool isKey(const QString& text) {
    if (text.size() != 32)
        return false;
    for (const QChar c : text) {
        if (!((c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9') ||
              c == u'-' || c == u'_'))
            return false;
    }
    return true;
}

qsizetype characters(const QString& text) { return text.toUcs4().size(); }

qsizetype utf8Bytes(const QString& text) { return text.toUtf8().size(); }

// RFC 3339 in UTC: `YYYY-MM-DDTHH:MM:SS[.fraction]Z`.
bool isUtcTime(const QString& text) {
    static const QRegularExpression pattern(
        QStringLiteral("^(\\d{4})-(\\d{2})-(\\d{2})T(\\d{2}):(\\d{2}):(\\d{2})(\\.\\d{1,9})?Z$"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch())
        return false;
    const auto part = [&match](const int index) { return match.captured(index).toInt(); };
    return QDate::isValid(part(1), part(2), part(3)) && QTime::isValid(part(4), part(5), part(6));
}

bool isEnvName(const QString& name) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    return pattern.match(name).hasMatch();
}

// A reason code; `allowEmpty` for `job.result.reason`.
bool isCode(const QString& code, const bool allowEmpty = false) {
    return (allowEmpty && code.isEmpty()) || rules::isReasonCode(code);
}

// Text that survives the conversion to bytes unchanged: no NUL and no unpaired surrogate.
bool isExactText(const QString& text) {
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c.isNull() || c.isLowSurrogate())
            return false;
        if (c.isHighSurrogate()) {
            if (i + 1 == text.size() || !text.at(i + 1).isLowSurrogate())
                return false;
            ++i;
        }
    }
    return true;
}

class Checker {
  public:
    explicit Checker(const QLatin1StringView type) : m_type(type) {}

    void require(const bool condition, const QLatin1StringView field) {
        if (!condition && !m_error)
            m_error = FrameError::of(FrameError::Kind::InvalidField, m_type, field);
    }
    void id(const QString& value, const QLatin1StringView field,
            const qsizetype maxChars = limits::maxIdChars) {
        require(rules::isToken(value, maxChars), field);
    }
    void fail(const FrameError::Kind kind) {
        if (!m_error)
            m_error = FrameError::of(kind, m_type);
    }
    void range(const qint64 value, const qint64 min, const qint64 max,
               const QLatin1StringView field) {
        require(value >= min && value <= max, field);
    }
    void offset(const qint64 value, const QLatin1StringView field) {
        range(value, 0, limits::maxSafeInteger, field);
    }
    void attempt(const qint64 value) {
        require(rules::isAttempt(value), QLatin1StringView("attempt"));
    }
    void jobId(const QString& value) { require(rules::isJobId(value), QLatin1StringView("jobId")); }
    void epoch(const qint64 value) { offset(value, QLatin1StringView("epoch")); }
    void objectId(const QString& value, const QLatin1StringView field) {
        require(isObjectId(value), field);
    }
    void code(const QString& value, const ErrorFrame frame) {
        const std::optional<ErrorCode> known = errorCodeFromString(value);
        require(isCode(value) && (!known || errorFrame(*known) == frame),
                QLatin1StringView("code"));
    }
    void warnings(const QStringList& values) {
        bool known = values.size() <= limits::maxWarnings;
        for (const QString& value : values)
            known = known && knownWarnings().contains(value);
        known = known && QSet<QString>(values.cbegin(), values.cend()).size() == values.size();
        require(known, QLatin1StringView("warnings"));
    }
    void capabilities(const QStringList& values) {
        bool valid = values.size() <= limits::maxCapabilities;
        for (const QString& value : values)
            valid = valid && !value.isEmpty() && isExactText(value) &&
                    characters(value) <= limits::maxCapabilityChars;
        require(valid, QLatin1StringView("capabilities"));
    }
    void oneOf(const QString& value, std::initializer_list<QLatin1StringView> allowed,
               const QLatin1StringView field) {
        bool found = false;
        for (const QLatin1StringView candidate : allowed)
            found = found || value == candidate;
        require(found, field);
    }

    [[nodiscard]] std::optional<FrameError> error() const { return m_error; }

  private:
    QString m_type;
    std::optional<FrameError> m_error;
};

void check(Checker& c, const Hello& f, Direction) {
    c.id(f.connectionId, QLatin1StringView("connectionId"));
    c.capabilities(f.capabilities);
    c.id(f.runnerId, QLatin1StringView("runnerId"));
    c.id(f.runnerVersion, QLatin1StringView("runnerVersion"), limits::maxRunnerVersionChars);
    c.id(f.os, QLatin1StringView("os"));
    c.id(f.arch, QLatin1StringView("arch"));
    bool labels = f.labels.size() <= limits::maxLabels;
    for (const QString& label : f.labels)
        labels = labels && rules::isToken(label, limits::maxLabelChars);
    c.require(labels, QLatin1StringView("labels"));
    c.jobId(f.jobId);
    c.attempt(f.attempt);
    bool snapshots = f.snapshots.size() <= limits::maxSnapshots;
    for (auto it = f.snapshots.cbegin(); it != f.snapshots.cend(); ++it)
        snapshots = snapshots && isKey(it.key()) && isObjectId(it.value());
    c.require(snapshots, QLatin1StringView("snapshots"));
    if (!f.capabilities.contains(capability::runnerProtocol))
        c.fail(FrameError::Kind::MissingCapability);
}

void check(Checker& c, const Welcome& f, Direction) {
    c.id(f.connectionId, QLatin1StringView("connectionId"));
    c.epoch(f.epoch);
    c.capabilities(f.capabilities);
    c.id(f.daemonVersion, QLatin1StringView("daemonVersion"));
}

// Empty is the worktree root; otherwise segments joined by "/", none empty, "." or "..".
bool isRelativePath(const QString& cwd) {
    if (cwd.isEmpty())
        return true;
    if (utf8Bytes(cwd) > limits::maxCwdBytes || !isExactText(cwd))
        return false;
    for (const QString& segment : cwd.split(u'/')) {
        if (segment.isEmpty() || segment == QLatin1StringView(".") ||
            segment == QLatin1StringView(".."))
            return false;
    }
    return true;
}

void checkInvocation(Checker& c, const Invocation& invocation) {
    const QLatin1StringView field("invocation");
    c.require(invocation.argv.has_value() != invocation.shell.has_value(), field);
    if (invocation.argv) {
        const QStringList& argv = *invocation.argv;
        bool valid = !argv.isEmpty() && argv.size() <= limits::maxArgv && !argv.front().isEmpty();
        for (const QString& argument : argv)
            valid = valid && isExactText(argument);
        c.require(valid, QLatin1StringView("invocation.argv"));
    }
    if (invocation.shell) {
        const QString& shell = *invocation.shell;
        c.require(!shell.isEmpty() && utf8Bytes(shell) <= limits::maxShellBytes &&
                      isExactText(shell),
                  QLatin1StringView("invocation.shell"));
    }
}

void checkEnv(Checker& c, const QMap<QString, std::optional<QString>>& env) {
    bool valid = env.size() <= limits::maxEnvNames;
    qsizetype bytes = 0;
    for (auto it = env.cbegin(); it != env.cend(); ++it) {
        valid = valid && isEnvName(it.key()) && !it.key().startsWith(QStringLiteral("KETPLUS_"));
        bytes += utf8Bytes(it.key());
        if (it.value()) {
            valid = valid && isExactText(*it.value());
            bytes += utf8Bytes(*it.value());
        }
    }
    c.require(valid && bytes <= limits::maxEnvBytes, QLatin1StringView("env"));
}

void check(Checker& c, const JobSpec& f, Direction) {
    c.epoch(f.epoch);
    c.jobId(f.jobId);
    c.attempt(f.attempt);
    checkInvocation(c, f.invocation);
    c.require(isRelativePath(f.cwd), QLatin1StringView("cwd"));
    checkEnv(c, f.env);
    c.range(f.timeoutMs, limits::minTimeoutMs, limits::maxTimeoutMs,
            QLatin1StringView("timeoutMs"));
    c.require(isKey(f.repositoryKey), QLatin1StringView("repositoryKey"));
    c.require(isKey(f.workspaceKey), QLatin1StringView("workspaceKey"));
    c.oneOf(f.objectFormat, {QLatin1StringView("sha1"), QLatin1StringView("sha256")},
            QLatin1StringView("objectFormat"));
    const qsizetype idLength = objectIdLength(f.objectFormat);
    c.require(isLowerHex(f.snapshotId, idLength), QLatin1StringView("snapshotId"));
    c.require(f.sourceHead.isEmpty() || isLowerHex(f.sourceHead, idLength),
              QLatin1StringView("sourceHead"));
}

void check(Checker& c, const JobAccepted& f, Direction) {
    c.jobId(f.jobId);
    c.attempt(f.attempt);
}

void check(Checker& c, const JobRejected& f, Direction) {
    c.jobId(f.jobId);
    c.attempt(f.attempt);
    c.code(f.code, ErrorFrame::JobRejected);
}

void check(Checker& c, const JobResume& f, Direction) {
    c.epoch(f.epoch);
    c.jobId(f.jobId);
    c.offset(f.outputOffset, QLatin1StringView("outputOffset"));
}

void check(Checker& c, const SnapshotOffer& f, Direction) {
    c.epoch(f.epoch);
    c.jobId(f.jobId);
    c.objectId(f.snapshotId, QLatin1StringView("snapshotId"));
    c.require(!f.baseId || isObjectId(*f.baseId), QLatin1StringView("baseId"));
    c.range(f.packBytes, 1, limits::maxSafeInteger, QLatin1StringView("packBytes"));
    c.require(isLowerHex(f.packSha256, 64), QLatin1StringView("packSha256"));
    c.require(f.chunkBytes == limits::chunkBytes, QLatin1StringView("chunkBytes"));
    c.warnings(f.warnings);
}

void check(Checker& c, const SnapshotWant& f, Direction) {
    c.objectId(f.snapshotId, QLatin1StringView("snapshotId"));
    c.oneOf(f.state, {QLatin1StringView("have"), QLatin1StringView("need")},
            QLatin1StringView("state"));
    c.offset(f.fromOffset, QLatin1StringView("fromOffset"));
    c.require(f.state != QLatin1StringView("have") || f.fromOffset == 0,
              QLatin1StringView("fromOffset"));
}

void check(Checker& c, const SnapshotChunk& f, Direction) {
    c.epoch(f.epoch);
    c.objectId(f.snapshotId, QLatin1StringView("snapshotId"));
    c.offset(f.offset, QLatin1StringView("offset"));
    c.range(f.bytes, 1, limits::chunkBytes, QLatin1StringView("bytes"));
    c.require(isLowerHex(f.sha256, 64), QLatin1StringView("sha256"));
    c.require(isLowerHex(f.transferId, 16), QLatin1StringView("transferId"));
}

void check(Checker& c, const SnapshotAck& f, Direction) {
    c.objectId(f.snapshotId, QLatin1StringView("snapshotId"));
    c.offset(f.offset, QLatin1StringView("offset"));
}

void check(Checker& c, const SnapshotReady& f, Direction) {
    c.objectId(f.snapshotId, QLatin1StringView("snapshotId"));
}

void check(Checker& c, const SnapshotError& f, Direction) {
    c.objectId(f.snapshotId, QLatin1StringView("snapshotId"));
    c.code(f.code, ErrorFrame::SnapshotError);
}

void check(Checker& c, const JobStarted& f, Direction) {
    c.jobId(f.jobId);
    c.require(isUtcTime(f.startedAt), QLatin1StringView("startedAt"));
}

void check(Checker& c, const JobOutput& f, Direction) {
    c.jobId(f.jobId);
    c.offset(f.offset, QLatin1StringView("offset"));
    c.require(!f.data.isEmpty() && f.data.size() <= limits::maxOutputBytes,
              QLatin1StringView("data"));
}

void check(Checker& c, const JobOutputGap& f, Direction) {
    c.jobId(f.jobId);
    c.offset(f.fromOffset, QLatin1StringView("fromOffset"));
    c.offset(f.toOffset, QLatin1StringView("toOffset"));
    c.require(f.fromOffset < f.toOffset, QLatin1StringView("toOffset"));
}

void check(Checker& c, const JobOutputAck& f, Direction) {
    c.epoch(f.epoch);
    c.jobId(f.jobId);
    c.offset(f.offset, QLatin1StringView("offset"));
}

void check(Checker& c, const JobOutputResume& f, Direction) {
    c.epoch(f.epoch);
    c.jobId(f.jobId);
    c.offset(f.fromOffset, QLatin1StringView("fromOffset"));
}

void check(Checker& c, const JobCancel& f, Direction) {
    c.epoch(f.epoch);
    c.jobId(f.jobId);
    c.oneOf(f.reason,
            {QLatin1StringView("user"), QLatin1StringView("deadline"),
             QLatin1StringView("session_closed")},
            QLatin1StringView("reason"));
}

void check(Checker& c, const JobResult& f, Direction) {
    c.jobId(f.jobId);
    c.attempt(f.attempt);
    c.oneOf(f.state,
            {QLatin1StringView("exited"), QLatin1StringView("cancelled"),
             QLatin1StringView("timed_out"), QLatin1StringView("interrupted"),
             QLatin1StringView("failed")},
            QLatin1StringView("state"));
    c.require(!f.exitCode ||
                  (*f.exitCode >= limits::minExitCode && *f.exitCode <= limits::maxExitCode),
              QLatin1StringView("exitCode"));
    c.require(!f.signal || (*f.signal >= 1 && *f.signal <= limits::maxSignal),
              QLatin1StringView("signal"));
    c.require(!(f.exitCode && f.signal), QLatin1StringView("signal"));
    c.require(isCode(f.reason, true), QLatin1StringView("reason"));
    c.oneOf(f.cleanupStatus, {QLatin1StringView("verified"), QLatin1StringView("unverified")},
            QLatin1StringView("cleanupStatus"));
    c.offset(f.outputBytes, QLatin1StringView("outputBytes"));
    c.require(!f.startedAt || isUtcTime(*f.startedAt), QLatin1StringView("startedAt"));
    c.require(!f.endedAt || isUtcTime(*f.endedAt), QLatin1StringView("endedAt"));
    c.warnings(f.warnings);
}

void check(Checker& c, const JobResultAck& f, Direction) {
    c.epoch(f.epoch);
    c.jobId(f.jobId);
}

// `epoch` is present exactly on the desktop's copy of an either-way frame.
void checkEpoch(Checker& c, const std::optional<qint64>& epoch, const Direction direction) {
    c.require(epoch.has_value() == (direction == Direction::DesktopToRunner),
              QLatin1StringView("epoch"));
    if (epoch)
        c.epoch(*epoch);
}

void check(Checker& c, const Ping& f, const Direction direction) {
    checkEpoch(c, f.epoch, direction);
    c.id(f.nonce, QLatin1StringView("nonce"), limits::maxNonceChars);
}

void check(Checker& c, const Pong& f, const Direction direction) {
    checkEpoch(c, f.epoch, direction);
    c.id(f.nonce, QLatin1StringView("nonce"), limits::maxNonceChars);
}

void check(Checker& c, const Close& f, const Direction direction) {
    checkEpoch(c, f.epoch, direction);
    c.require(isPrintableAscii(f.reason, limits::maxCodeChars), QLatin1StringView("reason"));
}

} // namespace

bool rules::isToken(const QString& text, const qsizetype maxChars) {
    if (text.isEmpty() || text.size() > maxChars)
        return false;
    for (const QChar c : text) {
        if (c.unicode() < 0x21 || c.unicode() > 0x7e)
            return false;
    }
    return true;
}

bool rules::isReasonCode(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z][a-z0-9_]{0,63}$"));
    static_assert(limits::maxCodeChars == 64);
    return pattern.match(text).hasMatch();
}

bool rules::isJobId(const QString& text) { return isToken(text, limits::maxIdChars); }

bool rules::isAttempt(const qint64 value) { return value >= 1 && value <= limits::maxAttempt; }

bool rules::isKey(const QString& text) { return protocol::isKey(text); }

bool rules::isObjectId(const QString& text) { return protocol::isObjectId(text); }

bool rules::isLowerHex(const QString& text, const qsizetype length) {
    return protocol::isLowerHex(text, length);
}

QLatin1StringView frameType(const Frame& frame) {
    return std::visit([](const auto& f) { return std::decay_t<decltype(f)>::type; }, frame);
}

std::optional<ErrorCode> errorCode(const JobRejected& frame) {
    return errorCodeFromString(frame.code);
}

std::optional<ErrorCode> errorCode(const SnapshotError& frame) {
    return errorCodeFromString(frame.code);
}

const QStringList& knownWarnings() {
    static const QStringList warnings{QStringLiteral("submodules_not_synced"),
                                      QStringLiteral("lfs_not_synced"),
                                      QStringLiteral("filtered_not_synced")};
    return warnings;
}

std::optional<FrameError> validate(const Frame& frame, const Direction direction) {
    const QLatin1StringView type = frameType(frame);
    const Route* route = routeFor(type);
    if (route == nullptr || !allows(*route, direction))
        return FrameError::of(FrameError::Kind::WrongDirection, type);
    Checker checker(type);
    std::visit([&checker, direction](const auto& f) { check(checker, f, direction); }, frame);
    return checker.error();
}

} // namespace ketplus::runner::protocol
