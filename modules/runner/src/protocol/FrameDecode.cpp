#include "protocol/FrameLimits.h"
#include "protocol/FrameRules.h"
#include "protocol/Frames.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringDecoder>

#include <limits>

namespace ketplus::runner::protocol {
namespace {

using L = QLatin1StringView;

// Reads typed fields from a frame object and keeps the first field that is missing or of the
// wrong JSON type. Range checks are validate()'s.
class Reader {
  public:
    Reader(const QJsonObject& object, const QLatin1StringView type)
        : m_object(object), m_type(type) {}

    QString string(const L field) {
        const QJsonValue value = m_object.value(field);
        require(value.isString(), field);
        return value.toString();
    }
    std::optional<QString> nullableString(const L field) {
        const QJsonValue value = m_object.value(field);
        if (value.isNull())
            return std::nullopt;
        require(value.isString(), field);
        return value.toString();
    }
    qint64 integer(const L field) { return integerValue(m_object.value(field), field); }
    std::optional<qint64> nullableInteger(const L field) {
        const QJsonValue value = m_object.value(field);
        if (value.isNull())
            return std::nullopt;
        return integerValue(value, field);
    }
    bool boolean(const L field) {
        const QJsonValue value = m_object.value(field);
        require(value.isBool(), field);
        return value.toBool();
    }
    QStringList strings(const L field) { return stringArray(m_object.value(field), field); }
    // At most limits::maxWarnings entries as received; repeats are dropped, first one kept.
    QStringList warnings(const L field) {
        const QStringList received = strings(field);
        require(received.size() <= limits::maxWarnings, field);
        QStringList unique;
        for (const QString& warning : received) {
            if (!unique.contains(warning))
                unique.append(warning);
        }
        return unique;
    }
    QMap<QString, QString> stringMap(const L field) {
        const QJsonValue value = m_object.value(field);
        require(value.isObject(), field);
        QMap<QString, QString> result;
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            require(it.value().isString(), field);
            result.insert(it.key(), it.value().toString());
        }
        return result;
    }
    QMap<QString, std::optional<QString>> nullableStringMap(const L field) {
        const QJsonValue value = m_object.value(field);
        require(value.isObject(), field);
        QMap<QString, std::optional<QString>> result;
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (it.value().isNull()) {
                result.insert(it.key(), std::nullopt);
                continue;
            }
            require(it.value().isString(), field);
            result.insert(it.key(), it.value().toString());
        }
        return result;
    }
    Invocation invocation() {
        const L field("invocation");
        const QJsonValue value = m_object.value(field);
        require(value.isObject(), field);
        const QJsonObject object = value.toObject();
        Invocation result;
        if (object.contains(L("argv")))
            result.argv = stringArray(object.value(L("argv")), L("invocation.argv"));
        if (object.contains(L("shell"))) {
            const QJsonValue shell = object.value(L("shell"));
            require(shell.isString(), L("invocation.shell"));
            result.shell = shell.toString();
        }
        return result;
    }
    // Standard base64 with padding; any other spelling of the same bytes is refused.
    QByteArray base64(const L field) {
        const QJsonValue value = m_object.value(field);
        require(value.isString(), field);
        const QByteArray text = value.toString().toLatin1();
        auto decoded =
            QByteArray::fromBase64Encoding(text, QByteArray::AbortOnBase64DecodingErrors);
        require(decoded && decoded.decoded.toBase64() == text, field);
        return decoded ? decoded.decoded : QByteArray();
    }

    [[nodiscard]] std::optional<FrameError> error() const { return m_error; }

  private:
    void require(const bool condition, const L field) {
        if (!condition && !m_error)
            m_error = FrameError::of(FrameError::Kind::InvalidField, m_type, field);
    }
    qint64 integerValue(const QJsonValue& value, const L field) {
        constexpr qint64 notIntegral = std::numeric_limits<qint64>::min();
        const qint64 result = value.isDouble() ? value.toInteger(notIntegral) : notIntegral;
        const bool valid = result != notIntegral && result >= -limits::maxSafeInteger &&
                           result <= limits::maxSafeInteger;
        require(valid, field);
        return valid ? result : 0;
    }
    QStringList stringArray(const QJsonValue& value, const L field) {
        require(value.isArray(), field);
        QStringList result;
        for (const QJsonValue item : value.toArray()) {
            require(item.isString(), field);
            result.append(item.toString());
        }
        return result;
    }

    const QJsonObject& m_object;
    QString m_type;
    std::optional<FrameError> m_error;
};

Frame read(Reader& r, Hello f, Direction) {
    f.connectionId = r.string(L("connectionId"));
    f.capabilities = r.strings(L("capabilities"));
    f.runnerId = r.string(L("runnerId"));
    f.runnerVersion = r.string(L("runnerVersion"));
    f.os = r.string(L("os"));
    f.arch = r.string(L("arch"));
    f.labels = r.strings(L("labels"));
    f.jobId = r.string(L("jobId"));
    f.attempt = r.integer(L("attempt"));
    f.snapshots = r.stringMap(L("snapshots"));
    return f;
}

Frame read(Reader& r, Welcome f, Direction) {
    f.connectionId = r.string(L("connectionId"));
    f.epoch = r.integer(L("epoch"));
    f.capabilities = r.strings(L("capabilities"));
    f.daemonVersion = r.string(L("daemonVersion"));
    return f;
}

Frame read(Reader& r, JobSpec f, Direction) {
    f.epoch = r.integer(L("epoch"));
    f.jobId = r.string(L("jobId"));
    f.attempt = r.integer(L("attempt"));
    f.invocation = r.invocation();
    f.cwd = r.string(L("cwd"));
    f.env = r.nullableStringMap(L("env"));
    f.timeoutMs = r.integer(L("timeoutMs"));
    f.repositoryKey = r.string(L("repositoryKey"));
    f.workspaceKey = r.string(L("workspaceKey"));
    f.snapshotId = r.string(L("snapshotId"));
    f.sourceHead = r.string(L("sourceHead"));
    f.objectFormat = r.string(L("objectFormat"));
    f.clean = r.boolean(L("clean"));
    return f;
}

Frame read(Reader& r, JobAccepted f, Direction) {
    f.jobId = r.string(L("jobId"));
    f.attempt = r.integer(L("attempt"));
    return f;
}

Frame read(Reader& r, JobRejected f, Direction) {
    f.jobId = r.string(L("jobId"));
    f.attempt = r.integer(L("attempt"));
    f.code = r.string(L("code"));
    return f;
}

Frame read(Reader& r, JobResume f, Direction) {
    f.epoch = r.integer(L("epoch"));
    f.jobId = r.string(L("jobId"));
    f.outputOffset = r.integer(L("outputOffset"));
    return f;
}

Frame read(Reader& r, SnapshotOffer f, Direction) {
    f.epoch = r.integer(L("epoch"));
    f.jobId = r.string(L("jobId"));
    f.snapshotId = r.string(L("snapshotId"));
    f.baseId = r.nullableString(L("baseId"));
    f.packBytes = r.integer(L("packBytes"));
    f.packSha256 = r.string(L("packSha256"));
    f.chunkBytes = r.integer(L("chunkBytes"));
    f.warnings = r.warnings(L("warnings"));
    return f;
}

Frame read(Reader& r, SnapshotWant f, Direction) {
    f.snapshotId = r.string(L("snapshotId"));
    f.state = r.string(L("state"));
    f.fromOffset = r.integer(L("fromOffset"));
    return f;
}

Frame read(Reader& r, SnapshotChunk f, Direction) {
    f.epoch = r.integer(L("epoch"));
    f.snapshotId = r.string(L("snapshotId"));
    f.offset = r.integer(L("offset"));
    f.bytes = r.integer(L("bytes"));
    f.sha256 = r.string(L("sha256"));
    f.transferId = r.string(L("transferId"));
    return f;
}

Frame read(Reader& r, SnapshotAck f, Direction) {
    f.snapshotId = r.string(L("snapshotId"));
    f.offset = r.integer(L("offset"));
    return f;
}

Frame read(Reader& r, SnapshotReady f, Direction) {
    f.snapshotId = r.string(L("snapshotId"));
    return f;
}

Frame read(Reader& r, SnapshotError f, Direction) {
    f.snapshotId = r.string(L("snapshotId"));
    f.code = r.string(L("code"));
    return f;
}

Frame read(Reader& r, JobStarted f, Direction) {
    f.jobId = r.string(L("jobId"));
    f.startedAt = r.string(L("startedAt"));
    return f;
}

Frame read(Reader& r, JobOutput f, Direction) {
    f.jobId = r.string(L("jobId"));
    f.offset = r.integer(L("offset"));
    f.data = r.base64(L("data"));
    return f;
}

Frame read(Reader& r, JobOutputGap f, Direction) {
    f.jobId = r.string(L("jobId"));
    f.fromOffset = r.integer(L("fromOffset"));
    f.toOffset = r.integer(L("toOffset"));
    return f;
}

Frame read(Reader& r, JobOutputAck f, Direction) {
    f.epoch = r.integer(L("epoch"));
    f.jobId = r.string(L("jobId"));
    f.offset = r.integer(L("offset"));
    return f;
}

Frame read(Reader& r, JobOutputResume f, Direction) {
    f.epoch = r.integer(L("epoch"));
    f.jobId = r.string(L("jobId"));
    f.fromOffset = r.integer(L("fromOffset"));
    return f;
}

Frame read(Reader& r, JobCancel f, Direction) {
    f.epoch = r.integer(L("epoch"));
    f.jobId = r.string(L("jobId"));
    f.reason = r.string(L("reason"));
    return f;
}

Frame read(Reader& r, JobResult f, Direction) {
    f.jobId = r.string(L("jobId"));
    f.attempt = r.integer(L("attempt"));
    f.state = r.string(L("state"));
    f.exitCode = r.nullableInteger(L("exitCode"));
    f.signal = r.nullableInteger(L("signal"));
    f.reason = r.string(L("reason"));
    f.cleanupStatus = r.string(L("cleanupStatus"));
    f.outputBytes = r.integer(L("outputBytes"));
    f.startedAt = r.nullableString(L("startedAt"));
    f.endedAt = r.nullableString(L("endedAt"));
    f.warnings = r.warnings(L("warnings"));
    return f;
}

Frame read(Reader& r, JobResultAck f, Direction) {
    f.epoch = r.integer(L("epoch"));
    f.jobId = r.string(L("jobId"));
    return f;
}

// Only the desktop's copy carries `epoch`; the runner's is ignored like any unknown field.
std::optional<qint64> readEpoch(Reader& r, const Direction direction) {
    if (direction == Direction::DesktopToRunner)
        return r.integer(L("epoch"));
    return std::nullopt;
}

Frame read(Reader& r, Ping f, const Direction direction) {
    f.epoch = readEpoch(r, direction);
    f.nonce = r.string(L("nonce"));
    return f;
}

Frame read(Reader& r, Pong f, const Direction direction) {
    f.epoch = readEpoch(r, direction);
    f.nonce = r.string(L("nonce"));
    return f;
}

Frame read(Reader& r, Close f, const Direction direction) {
    f.epoch = readEpoch(r, direction);
    f.reason = r.string(L("reason"));
    return f;
}

template <std::size_t Index = 0>
std::optional<Frame> readAs(const QStringView type, Reader& reader, const Direction direction) {
    if constexpr (Index < std::variant_size_v<Frame>) {
        using Type = std::variant_alternative_t<Index, Frame>;
        if (type == Type::type)
            return read(reader, Type{}, direction);
        return readAs<Index + 1>(type, reader, direction);
    } else {
        return std::nullopt;
    }
}

FrameError failure(const FrameError::Kind kind, const QString& type = {}) {
    return FrameError::of(kind, type);
}

// Attaches the `jobId` and `attempt` of a refused `job.spec` when both are themselves valid.
void identifySpec(const QJsonObject& object, FrameError& error) {
    const QJsonValue jobId = object.value(L("jobId"));
    const QJsonValue attempt = object.value(L("attempt"));
    constexpr qint64 notIntegral = std::numeric_limits<qint64>::min();
    const qint64 attemptValue = attempt.isDouble() ? attempt.toInteger(notIntegral) : notIntegral;
    if (!jobId.isString() || !rules::isJobId(jobId.toString()) || !rules::isAttempt(attemptValue))
        return;
    error.jobId = jobId.toString();
    error.attempt = attemptValue;
}

bool isUtf8(const QByteArray& message) {
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = decoder.decode(message);
    Q_UNUSED(text);
    return !decoder.hasError();
}

} // namespace

DecodeResult decode(const QByteArray& message, const Channel channel, const Direction direction) {
    using Kind = FrameError::Kind;
    // No frame may exceed the largest limit; refuse before parsing it.
    if (message.size() > limits::maxFrameBytes)
        return {std::nullopt, failure(Kind::Oversize)};
    if (!isUtf8(message))
        return {std::nullopt, failure(Kind::InvalidUtf8)};
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(message, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return {std::nullopt, failure(Kind::NotJson)};
    if (!document.isObject())
        return {std::nullopt, failure(Kind::NotObject)};
    const QJsonObject object = document.object();
    const QJsonValue typeValue = object.value(L("type"));
    if (!typeValue.isString())
        return {std::nullopt, failure(Kind::MissingType)};
    const QString type = typeValue.toString();
    const Route* route = routeFor(type);
    if (route == nullptr)
        return {std::nullopt, failure(Kind::UnsupportedType, type)};
    if (route->channel != channel)
        return {std::nullopt, failure(Kind::WrongChannel, type)};
    if (!allows(*route, direction))
        return {std::nullopt, failure(Kind::WrongDirection, type)};
    if (message.size() > route->maxBytes)
        return {std::nullopt, failure(Kind::Oversize, type)};

    Reader reader(object, route->type);
    std::optional<Frame> frame = readAs(type, reader, direction);
    std::optional<FrameError> error = reader.error();
    if (!error)
        error = validate(*frame, direction);
    if (!error)
        return {std::move(frame), {}};
    if (error->kind == Kind::InvalidField && type == JobSpec::type)
        identifySpec(object, *error);
    return {std::nullopt, *error};
}

} // namespace ketplus::runner::protocol
