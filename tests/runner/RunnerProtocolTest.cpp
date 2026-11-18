#include "RunnerProtocolTest.h"

#include "protocol/Capabilities.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTest>

using namespace runner_protocol_test;

namespace runner_protocol_test {

QString objectId(const char digit) { return QString(40, QLatin1Char(digit)); }

QString key(const char digit) { return QString(30, QLatin1Char(digit)) + QStringLiteral("-_"); }

QString sha256(const char digit) { return QString(64, QLatin1Char(digit)); }

QByteArray compact(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

Direction senderOf(const Frame& frame) {
    const Route* route = routeFor(frameType(frame));
    return route->fromRunner ? Direction::RunnerToDesktop : Direction::DesktopToRunner;
}

QList<Sample> samples() {
    const auto r = Direction::RunnerToDesktop;
    const auto d = Direction::DesktopToRunner;
    JobSpec spec;
    spec.epoch = 3;
    spec.jobId = QStringLiteral("job-1");
    spec.attempt = 2;
    spec.invocation.argv = QStringList{QStringLiteral("make"), QStringLiteral("-j4")};
    spec.cwd = QStringLiteral("build/out");
    spec.env = {{QStringLiteral("CC"), QStringLiteral("clang")},
                {QStringLiteral("NO_COLOR"), std::nullopt}};
    spec.timeoutMs = 60000;
    spec.repositoryKey = key('r');
    spec.workspaceKey = key('w');
    spec.snapshotId = objectId('a');
    spec.sourceHead = objectId('b');
    spec.objectFormat = QStringLiteral("sha1");
    spec.clean = true;

    JobResult result;
    result.jobId = QStringLiteral("job-1");
    result.attempt = 2;
    result.state = QStringLiteral("exited");
    result.exitCode = 1;
    result.signal = std::nullopt;
    result.reason = QStringLiteral("exit_status");
    result.cleanupStatus = QStringLiteral("verified");
    result.outputBytes = 1234;
    result.startedAt = QStringLiteral("2026-11-18T01:02:03Z");
    result.endedAt = QStringLiteral("2026-11-18T01:02:04.250Z");
    result.warnings = {QStringLiteral("lfs_not_synced")};

    return {
        {Hello{QStringLiteral("conn-1"),
               {QString(capability::runnerProtocol), QStringLiteral("future/2")},
               QStringLiteral("runner-1"),
               QStringLiteral("1.4.0"),
               QStringLiteral("linux"),
               QStringLiteral("x86_64"),
               {QStringLiteral("os:linux"), QStringLiteral("gpu")},
               QStringLiteral("job-1"),
               2,
               {{key('w'), objectId('c')}}},
         r},
        {Welcome{QStringLiteral("conn-1"),
                 3,
                 {QString(capability::runnerProtocol)},
                 QStringLiteral("0.30.0")},
         d},
        {spec, d},
        {JobAccepted{QStringLiteral("job-1"), 2}, r},
        {JobRejected{QStringLiteral("job-1"), 2, QStringLiteral("runner_busy")}, r},
        {JobResume{3, QStringLiteral("job-1"), 4096}, d},
        {SnapshotOffer{
             3,
             QStringLiteral("job-1"),
             objectId('a'),
             objectId('c'),
             9000000,
             sha256('e'),
             4194304,
             {QStringLiteral("submodules_not_synced"), QStringLiteral("filtered_not_synced")}},
         d},
        {SnapshotWant{objectId('a'), QStringLiteral("need"), 4194304}, r},
        {SnapshotChunk{3, objectId('a'), 4194304, 4194304, sha256('d'),
                       QStringLiteral("0123456789abcdef")},
         d},
        {SnapshotAck{objectId('a'), 8388608}, r},
        {SnapshotReady{objectId('a')}, r},
        {SnapshotError{objectId('a'), QStringLiteral("base_missing")}, r},
        {JobStarted{QStringLiteral("job-1"), QStringLiteral("2026-11-18T01:02:03Z")}, r},
        {JobOutput{QStringLiteral("job-1"), 100, QByteArray("hello\n\x00\xff", 8)}, r},
        {JobOutputGap{QStringLiteral("job-1"), 100, 200}, r},
        {JobOutputAck{3, QStringLiteral("job-1"), 200}, d},
        {JobOutputResume{3, QStringLiteral("job-1"), 200}, d},
        {JobCancel{3, QStringLiteral("job-1"), QStringLiteral("deadline")}, d},
        {result, r},
        {JobResultAck{3, QStringLiteral("job-1")}, d},
        {Ping{std::nullopt, QStringLiteral("n-1")}, r},
        {Pong{3, QStringLiteral("n-1")}, d},
        {Close{std::nullopt, QStringLiteral("superseded")}, r},
    };
}

} // namespace runner_protocol_test

namespace {

Channel channelOf(const Frame& frame) { return routeFor(frameType(frame))->channel; }

Direction opposite(const Direction direction) {
    return direction == Direction::RunnerToDesktop ? Direction::DesktopToRunner
                                                   : Direction::RunnerToDesktop;
}

const Sample& sampleOf(const QLatin1StringView type) {
    static const QList<Sample> all = samples();
    for (const Sample& sample : all) {
        if (frameType(sample.frame) == type)
            return sample;
    }
    qFatal("no sample");
}

void addRoundTripRow(const char* name, const Frame& frame, const Direction direction) {
    QTest::newRow(name) << frame << direction;
}

} // namespace

void RunnerProtocolTest::everyFrameRoundTrips_data() {
    QTest::addColumn<Frame>("frame");
    QTest::addColumn<Direction>("direction");
    QSet<QString> covered;
    for (const Sample& sample : samples()) {
        covered.insert(frameType(sample.frame));
        QTest::newRow(frameType(sample.frame).data()) << sample.frame << sample.direction;
    }
    // Every frame of the routing table has a sample.
    QCOMPARE(covered.size(), qsizetype(allRoutes().size()));
}

void RunnerProtocolTest::everyFrameRoundTrips() {
    QFETCH(Frame, frame);
    QFETCH(Direction, direction);
    const EncodeResult encoded = encode(frame, direction);
    QVERIFY2(encoded.ok(), qPrintable(encoded.error ? encoded.error->field : QString()));
    QCOMPARE(QJsonDocument::fromJson(encoded.message).object().value("type").toString(),
             QString(frameType(frame)));
    const DecodeResult decoded = decode(encoded.message, channelOf(frame), direction);
    QVERIFY2(decoded.ok(), qPrintable(decoded.error.field));
    QVERIFY(*decoded.frame == frame);
}

void RunnerProtocolTest::optionalAndNullFieldsRoundTrip_data() {
    QTest::addColumn<Frame>("frame");
    QTest::addColumn<Direction>("direction");
    const auto r = Direction::RunnerToDesktop;
    const auto d = Direction::DesktopToRunner;

    JobSpec shell = std::get<JobSpec>(sampleOf(JobSpec::type).frame);
    shell.invocation = Invocation{std::nullopt, QStringLiteral("make test && echo done")};
    shell.cwd.clear();
    shell.env.clear();
    shell.sourceHead.clear();
    shell.objectFormat = QStringLiteral("sha256");
    shell.snapshotId = QString(64, u'a');
    shell.clean = false;
    addRoundTripRow("job.spec shell, root cwd, no env, no head", shell, d);

    JobSpec unset = std::get<JobSpec>(sampleOf(JobSpec::type).frame);
    unset.env = {{QStringLiteral("ONLY_UNSET"), std::nullopt}};
    addRoundTripRow("job.spec env null unsets", unset, d);

    SnapshotOffer full = std::get<SnapshotOffer>(sampleOf(SnapshotOffer::type).frame);
    full.baseId = std::nullopt;
    full.warnings.clear();
    addRoundTripRow("snapshot.offer null baseId, no warnings", full, d);

    JobResult interrupted = std::get<JobResult>(sampleOf(JobResult::type).frame);
    interrupted.state = QStringLiteral("interrupted");
    interrupted.exitCode = std::nullopt;
    interrupted.signal = std::nullopt;
    interrupted.startedAt = std::nullopt;
    interrupted.endedAt = std::nullopt;
    interrupted.reason.clear();
    interrupted.cleanupStatus = QStringLiteral("unverified");
    interrupted.warnings.clear();
    addRoundTripRow("job.result all nullable null", interrupted, r);

    JobResult signalled = std::get<JobResult>(sampleOf(JobResult::type).frame);
    signalled.state = QStringLiteral("cancelled");
    signalled.exitCode = std::nullopt;
    signalled.signal = 9;
    addRoundTripRow("job.result signal", signalled, r);

    Hello bare = std::get<Hello>(sampleOf(Hello::type).frame);
    bare.labels.clear();
    bare.snapshots.clear();
    bare.capabilities = {QString(capability::runnerProtocol)};
    addRoundTripRow("hello no labels or snapshots", bare, r);

    addRoundTripRow("snapshot.want have", SnapshotWant{objectId('a'), QStringLiteral("have"), 0},
                    r);
    addRoundTripRow("ping from desktop", Ping{0, QStringLiteral("n")}, d);
    addRoundTripRow("pong from runner", Pong{std::nullopt, QStringLiteral("n")}, r);
    addRoundTripRow("close from desktop, empty reason", Close{7, QString()}, d);
}

void RunnerProtocolTest::optionalAndNullFieldsRoundTrip() { everyFrameRoundTrips(); }

void RunnerProtocolTest::routesMatchTheChannelTable() {
    // The Channels and Frames tables of the protocol: channel, and who sends.
    struct Expected {
        const char* type;
        Channel channel;
        bool runner;
        bool desktop;
        qsizetype maxBytes;
    };
    const auto c = Channel::Control;
    const auto e = Channel::Events;
    const qsizetype k = 1024;
    const QList<Expected> table{
        {"hello", c, true, false, 64 * k},
        {"welcome", c, false, true, 4 * k},
        {"job.spec", c, false, true, 64 * k},
        {"job.accepted", c, true, false, k},
        {"job.rejected", c, true, false, k},
        {"job.resume", c, false, true, k},
        {"snapshot.offer", c, false, true, 4 * k},
        {"snapshot.want", c, true, false, k},
        {"snapshot.chunk", c, false, true, 64 * k},
        {"snapshot.ack", c, true, false, k},
        {"snapshot.ready", c, true, false, k},
        {"snapshot.error", c, true, false, k},
        {"job.started", c, true, false, k},
        {"job.output", e, true, false, 64 * k},
        {"job.output.gap", e, true, false, k},
        {"job.output.ack", c, false, true, k},
        {"job.output.resume", c, false, true, k},
        {"job.cancel", c, false, true, k},
        {"job.result", c, true, false, 16 * k},
        {"job.result.ack", c, false, true, k},
        {"ping", c, true, true, k},
        {"pong", c, true, true, k},
        {"close", c, true, true, k},
    };
    QCOMPARE(qsizetype(allRoutes().size()), table.size());
    for (const Expected& row : table) {
        const Route* route = routeFor(QString::fromLatin1(row.type));
        QVERIFY2(route != nullptr, row.type);
        QVERIFY2(route->channel == row.channel, row.type);
        QVERIFY2(route->fromRunner == row.runner, row.type);
        QVERIFY2(route->fromDesktop == row.desktop, row.type);
        QVERIFY2(route->maxBytes == row.maxBytes, row.type);
    }
    QVERIFY(routeFor(u"job.output.unknown") == nullptr);
}

void RunnerProtocolTest::decodeRefusesWrongChannelAndDirection() {
    for (const Sample& sample : samples()) {
        const QByteArray message = compact(detail::toObject(sample.frame));
        const Channel home = channelOf(sample.frame);
        for (const Channel channel : {Channel::Control, Channel::Events, Channel::Bulk}) {
            if (channel == home)
                continue;
            const DecodeResult result = decode(message, channel, sample.direction);
            QVERIFY(!result.ok());
            QCOMPARE(result.error.kind, FrameError::Kind::WrongChannel);
            QCOMPARE(result.error.type, QString(frameType(sample.frame)));
        }
        const Route* route = routeFor(frameType(sample.frame));
        if (route->fromRunner && route->fromDesktop)
            continue;
        const DecodeResult reversed = decode(message, home, opposite(sample.direction));
        QVERIFY(!reversed.ok());
        QCOMPARE(reversed.error.kind, FrameError::Kind::WrongDirection);
        // The sender refuses it too.
        const EncodeResult encoded = encode(sample.frame, opposite(sample.direction));
        QVERIFY(!encoded.ok());
        QCOMPARE(encoded.error->kind, FrameError::Kind::WrongDirection);
    }
}

void RunnerProtocolTest::bulkCarriesNoJsonFrame() {
    for (const Route& route : allRoutes())
        QVERIFY(route.channel != Channel::Bulk);
}

void RunnerProtocolTest::epochFollowsTheSender() {
    // A desktop frame without epoch is refused.
    QJsonObject spec = detail::toObject(sampleOf(JobSpec::type).frame);
    spec.remove("epoch");
    DecodeResult result = decode(compact(spec), Channel::Control, Direction::DesktopToRunner);
    QCOMPARE(result.error.kind, FrameError::Kind::InvalidField);
    QCOMPARE(result.error.field, QStringLiteral("epoch"));

    // An either-way frame needs epoch from the desktop...
    const QByteArray bare = compact(detail::toObject(Ping{std::nullopt, QStringLiteral("n")}));
    result = decode(bare, Channel::Control, Direction::DesktopToRunner);
    QCOMPARE(result.error.kind, FrameError::Kind::InvalidField);
    QCOMPARE(result.error.field, QStringLiteral("epoch"));
    // ...and the runner's epoch is ignored.
    const QByteArray withEpoch = compact(detail::toObject(Ping{9, QStringLiteral("n")}));
    result = decode(withEpoch, Channel::Control, Direction::RunnerToDesktop);
    QVERIFY(result.ok());
    QVERIFY(!std::get<Ping>(*result.frame).epoch.has_value());
    // The runner never sends one.
    const EncodeResult encoded = encode(Close{9, QString()}, Direction::RunnerToDesktop);
    QCOMPARE(encoded.error->field, QStringLiteral("epoch"));
    QVERIFY(!encode(Close{std::nullopt, QString()}, Direction::DesktopToRunner).ok());
}

void RunnerProtocolTest::malformedInputIsRefused_data() {
    QTest::addColumn<QByteArray>("message");
    QTest::addColumn<FrameError::Kind>("kind");
    using Kind = FrameError::Kind;
    QTest::newRow("empty") << QByteArray() << Kind::NotJson;
    QTest::newRow("text") << QByteArray("hello") << Kind::NotJson;
    QTest::newRow("truncated") << QByteArray(R"({"type":"ping","nonce":)") << Kind::NotJson;
    QTest::newRow("trailing garbage") << QByteArray(R"({"type":"ping"} x)") << Kind::NotJson;
    QTest::newRow("array") << QByteArray(R"([{"type":"ping"}])") << Kind::NotObject;
    QTest::newRow("empty array") << QByteArray("[]") << Kind::NotObject;
    QTest::newRow("no type") << QByteArray(R"({"nonce":"n"})") << Kind::MissingType;
    QTest::newRow("numeric type") << QByteArray(R"({"type":1})") << Kind::MissingType;
    QTest::newRow("null type") << QByteArray(R"({"type":null})") << Kind::MissingType;
    QTest::newRow("invalid byte") << QByteArray("{\"type\":\"ping\",\"nonce\":\"\xff\"}")
                                  << Kind::InvalidUtf8;
    QTest::newRow("overlong") << QByteArray("{\"type\":\"ping\",\"nonce\":\"\xc0\xaf\"}")
                              << Kind::InvalidUtf8;
    QTest::newRow("surrogate") << QByteArray("{\"type\":\"ping\",\"nonce\":\"\xed\xa0\x80\"}")
                               << Kind::InvalidUtf8;
    QTest::newRow("truncated sequence")
        << QByteArray("{\"type\":\"ping\",\"nonce\":\"\xe2\x82\"}") << Kind::InvalidUtf8;
}

void RunnerProtocolTest::malformedInputIsRefused() {
    QFETCH(QByteArray, message);
    QFETCH(FrameError::Kind, kind);
    for (const Direction direction : {Direction::RunnerToDesktop, Direction::DesktopToRunner}) {
        const DecodeResult result = decode(message, Channel::Control, direction);
        QVERIFY(!result.ok());
        QCOMPARE(result.error.kind, kind);
    }
}

void RunnerProtocolTest::wrongFieldTypesAreRefused_data() {
    QTest::addColumn<QByteArray>("message");
    QTest::addColumn<Direction>("direction");
    QTest::addColumn<QString>("field");
    const auto mutated = [](const QLatin1StringView type, const char* field,
                            const QJsonValue& value) {
        const Sample& sample = sampleOf(type);
        QJsonObject object = detail::toObject(sample.frame);
        if (value.isUndefined())
            object.remove(QLatin1StringView(field));
        else
            object.insert(QLatin1StringView(field), value);
        return compact(object);
    };
    const auto row = [&](const char* name, const QLatin1StringView type, const char* field,
                         const QJsonValue& value, const char* expected) {
        QTest::newRow(name) << mutated(type, field, value) << senderOf(sampleOf(type).frame)
                            << QString::fromLatin1(expected);
    };
    const QJsonValue missing(QJsonValue::Undefined);
    row("missing jobId", JobAccepted::type, "jobId", missing, "jobId");
    row("string attempt", JobAccepted::type, "attempt", "2", "attempt");
    row("fractional attempt", JobAccepted::type, "attempt", 1.5, "attempt");
    row("zero attempt", JobAccepted::type, "attempt", 0, "attempt");
    row("bool attempt", JobAccepted::type, "attempt", true, "attempt");
    row("unsafe integer", JobOutputAck::type, "offset", 1e300, "offset");
    row("negative offset", JobOutputAck::type, "offset", -1, "offset");
    row("null jobId", JobResultAck::type, "jobId", QJsonValue::Null, "jobId");
    row("numeric jobId", JobResultAck::type, "jobId", 7, "jobId");
    row("empty jobId", JobResultAck::type, "jobId", "", "jobId");
    row("string clean", JobSpec::type, "clean", "true", "clean");
    row("env not object", JobSpec::type, "env", QJsonArray{}, "env");
    row("env numeric value", JobSpec::type, "env", QJsonObject{{"A", 1}}, "env");
    row("argv not array", JobSpec::type, "invocation", QJsonObject{{"argv", "make"}},
        "invocation.argv");
    row("argv numeric item", JobSpec::type, "invocation", QJsonObject{{"argv", QJsonArray{1}}},
        "invocation.argv");
    row("shell not string", JobSpec::type, "invocation", QJsonObject{{"shell", 1}},
        "invocation.shell");
    row("argv and shell", JobSpec::type, "invocation",
        QJsonObject{{"argv", QJsonArray{"a"}}, {"shell", "a"}}, "invocation");
    row("no invocation kind", JobSpec::type, "invocation", QJsonObject{}, "invocation");
    row("invocation not object", JobSpec::type, "invocation", "make", "invocation");
    row("labels item", Hello::type, "labels", QJsonArray{1}, "labels");
    row("snapshots value", Hello::type, "snapshots", QJsonObject{{key('w'), 1}}, "snapshots");
    row("snapshots array", Hello::type, "snapshots", QJsonArray{}, "snapshots");
    row("missing baseId", SnapshotOffer::type, "baseId", missing, "baseId");
    row("numeric baseId", SnapshotOffer::type, "baseId", 1, "baseId");
    row("null exitCode type", JobResult::type, "exitCode", "1", "exitCode");
    row("fractional exitCode", JobResult::type, "exitCode", 1.5, "exitCode");
    row("missing startedAt", JobResult::type, "startedAt", missing, "startedAt");
    row("data not base64", JobOutput::type, "data", "@@@@", "data");
    row("data unpadded", JobOutput::type, "data", "YQ", "data");
    row("data base64url", JobOutput::type, "data", "-_-_", "data");
    row("data number", JobOutput::type, "data", 1, "data");
    row("uppercase snapshotId", SnapshotReady::type, "snapshotId", objectId('A'), "snapshotId");
    row("unknown state", SnapshotWant::type, "state", "maybe", "state");
    row("unknown result state", JobResult::type, "state", "done", "state");
    row("bad time", JobStarted::type, "startedAt", "yesterday", "startedAt");
}

void RunnerProtocolTest::wrongFieldTypesAreRefused() {
    QFETCH(QByteArray, message);
    QFETCH(Direction, direction);
    QFETCH(QString, field);
    const QString type = QJsonDocument::fromJson(message).object().value("type").toString();
    const DecodeResult result = decode(message, routeFor(type)->channel, direction);
    QVERIFY(!result.ok());
    QCOMPARE(result.error.kind, FrameError::Kind::InvalidField);
    QCOMPARE(result.error.type, type);
    QCOMPARE(result.error.field, field);
}

void RunnerProtocolTest::unknownFieldsAreIgnored() {
    for (const Sample& sample : samples()) {
        QJsonObject object = detail::toObject(sample.frame);
        object.insert("futureField", QJsonObject{{"nested", QJsonArray{1, "two", true}}});
        object.insert("another", QJsonValue::Null);
        const DecodeResult result =
            decode(compact(object), channelOf(sample.frame), sample.direction);
        QVERIFY2(result.ok(), frameType(sample.frame).data());
        QVERIFY(*result.frame == sample.frame);
    }
}

void RunnerProtocolTest::unknownTypesAreUnsupported() {
    const QByteArray message(R"({"type":"job.future","jobId":"j"})");
    for (const Channel channel : {Channel::Control, Channel::Events, Channel::Bulk}) {
        for (const Direction direction : {Direction::RunnerToDesktop, Direction::DesktopToRunner}) {
            const DecodeResult result = decode(message, channel, direction);
            QVERIFY(!result.ok());
            QCOMPARE(result.error.kind, FrameError::Kind::UnsupportedType);
            QCOMPARE(result.error.type, QStringLiteral("job.future"));
        }
    }
    // Types are case-sensitive.
    QCOMPARE(decode(R"({"type":"Ping","nonce":"n"})", Channel::Control, Direction::RunnerToDesktop)
                 .error.kind,
             FrameError::Kind::UnsupportedType);
}

void RunnerProtocolTest::errorCodesConvertBothWays() {
    // The Error codes table of the protocol, in order, with the frame each belongs to.
    const QList<std::pair<QString, ErrorFrame>> table{
        {"not_paired", ErrorFrame::JobRejected},
        {"job_mismatch", ErrorFrame::JobRejected},
        {"already_dispatched", ErrorFrame::JobRejected},
        {"label_mismatch", ErrorFrame::JobRejected},
        {"runner_busy", ErrorFrame::JobRejected},
        {"spec_invalid", ErrorFrame::JobRejected},
        {"containment_unavailable", ErrorFrame::JobRejected},
        {"unsupported_frame", ErrorFrame::JobRejected},
        {"lease_lost", ErrorFrame::JobRejected},
        {"runner_unavailable", ErrorFrame::JobRejected},
        {"base_missing", ErrorFrame::SnapshotError},
        {"snapshot_too_large", ErrorFrame::SnapshotError},
        {"chunk_corrupt", ErrorFrame::SnapshotError},
        {"pack_invalid", ErrorFrame::SnapshotError},
        {"disk_full", ErrorFrame::SnapshotError},
        {"checkout_failed", ErrorFrame::SnapshotError},
    };
    QCOMPARE(qsizetype(allErrorCodes().size()), table.size());
    for (qsizetype i = 0; i < table.size(); ++i) {
        const ErrorCode code = allErrorCodes()[std::size_t(i)];
        QCOMPARE(QString(toString(code)), table.at(i).first);
        QCOMPARE(errorCodeFromString(table.at(i).first), std::optional<ErrorCode>(code));
        QVERIFY(errorFrame(code) == table.at(i).second);
    }
    QCOMPARE(errorCodeFromString(u"NOT_PAIRED"), std::nullopt);
    QCOMPARE(errorCodeFromString(u""), std::nullopt);
    QCOMPARE(errorCodeFromString(u"queue_full"), std::nullopt);
}

void RunnerProtocolTest::errorCodesBelongToTheirFrame() {
    const auto r = Direction::RunnerToDesktop;
    QCOMPARE(validate(JobRejected{"j", 1, "base_missing"}, r)->field, QStringLiteral("code"));
    QCOMPARE(validate(SnapshotError{objectId(), "runner_busy"}, r)->field, QStringLiteral("code"));
    QCOMPARE(validate(JobRejected{"j", 1, "Not_Paired"}, r)->field, QStringLiteral("code"));
    QCOMPARE(validate(JobRejected{"j", 1, ""}, r)->field, QStringLiteral("code"));
    // A code from a newer peer passes and has no enum value.
    const JobRejected future{"j", 1, "future_code"};
    QVERIFY(!validate(future, r).has_value());
    QCOMPARE(errorCode(future), std::nullopt);
    QCOMPARE(errorCode(JobRejected{"j", 1, "spec_invalid"}),
             std::optional<ErrorCode>(ErrorCode::SpecInvalid));
    QCOMPARE(errorCode(SnapshotError{objectId(), "disk_full"}),
             std::optional<ErrorCode>(ErrorCode::DiskFull));
}

void RunnerProtocolTest::helloWithoutTheProtocolCapability() {
    Hello hello = std::get<Hello>(sampleOf(Hello::type).frame);
    hello.capabilities = {QStringLiteral("runner.protocol/2")};
    const QByteArray message = compact(detail::toObject(hello));
    const DecodeResult result = decode(message, Channel::Control, Direction::RunnerToDesktop);
    QVERIFY(!result.ok());
    QCOMPARE(result.error.kind, FrameError::Kind::MissingCapability);
    QCOMPARE(encode(hello, Direction::RunnerToDesktop).error->kind,
             FrameError::Kind::MissingCapability);
    // `welcome` does not require it.
    Welcome welcome = std::get<Welcome>(sampleOf(Welcome::type).frame);
    welcome.capabilities.clear();
    QVERIFY(encode(welcome, Direction::DesktopToRunner).ok());
}

void RunnerProtocolTest::invalidSpecCarriesItsJobIdentity() {
    QJsonObject spec = detail::toObject(sampleOf(JobSpec::type).frame);
    spec.insert("cwd", "a/../b");
    DecodeResult result = decode(compact(spec), Channel::Control, Direction::DesktopToRunner);
    QCOMPARE(result.error.kind, FrameError::Kind::InvalidField);
    QCOMPARE(result.error.field, QStringLiteral("cwd"));
    QCOMPARE(result.error.jobId, std::optional<QString>(QStringLiteral("job-1")));
    QCOMPARE(result.error.attempt, std::optional<qint64>(2));

    // A wrong JSON type is answerable too.
    spec.insert("cwd", 1);
    result = decode(compact(spec), Channel::Control, Direction::DesktopToRunner);
    QCOMPARE(result.error.field, QStringLiteral("cwd"));
    QCOMPARE(result.error.jobId, std::optional<QString>(QStringLiteral("job-1")));

    // Without a valid jobId or attempt the spec cannot be answered.
    QJsonObject noJob = spec;
    noJob.remove("jobId");
    result = decode(compact(noJob), Channel::Control, Direction::DesktopToRunner);
    QCOMPARE(result.error.kind, FrameError::Kind::InvalidField);
    QCOMPARE(result.error.field, QStringLiteral("jobId"));
    QVERIFY(!result.error.jobId && !result.error.attempt);
    QJsonObject badAttempt = spec;
    badAttempt.insert("attempt", 0);
    result = decode(compact(badAttempt), Channel::Control, Direction::DesktopToRunner);
    QVERIFY(!result.error.jobId && !result.error.attempt);

    // Other frames, sizes, channels and directions never carry it.
    QJsonObject cancel = detail::toObject(sampleOf(JobCancel::type).frame);
    cancel.insert("reason", "bored");
    result = decode(compact(cancel), Channel::Control, Direction::DesktopToRunner);
    QCOMPARE(result.error.field, QStringLiteral("reason"));
    QVERIFY(!result.error.jobId && !result.error.attempt);
    const QByteArray valid = compact(detail::toObject(sampleOf(JobSpec::type).frame));
    result = decode(valid, Channel::Events, Direction::DesktopToRunner);
    QCOMPARE(result.error.kind, FrameError::Kind::WrongChannel);
    QVERIFY(!result.error.jobId);
    result = decode(valid, Channel::Control, Direction::RunnerToDesktop);
    QCOMPARE(result.error.kind, FrameError::Kind::WrongDirection);
    QVERIFY(!result.error.jobId);
}

void RunnerProtocolTest::duplicateWarningsAreDropped() {
    QJsonObject offer = detail::toObject(sampleOf(SnapshotOffer::type).frame);
    offer.insert("warnings", QJsonArray{"lfs_not_synced", "submodules_not_synced", "lfs_not_synced",
                                        "lfs_not_synced"});
    const DecodeResult result =
        decode(compact(offer), Channel::Control, Direction::DesktopToRunner);
    QVERIFY(result.ok());
    QCOMPARE(std::get<SnapshotOffer>(*result.frame).warnings,
             (QStringList{"lfs_not_synced", "submodules_not_synced"}));
    // The limit counts received entries, repeats included.
    offer.insert("warnings", QJsonArray::fromStringList(QStringList(8, "lfs_not_synced")));
    QVERIFY(decode(compact(offer), Channel::Control, Direction::DesktopToRunner).ok());
    offer.insert("warnings", QJsonArray::fromStringList(QStringList(9, "lfs_not_synced")));
    QCOMPARE(decode(compact(offer), Channel::Control, Direction::DesktopToRunner).error.field,
             QStringLiteral("warnings"));
    // Senders never repeat one.
    SnapshotOffer repeated = std::get<SnapshotOffer>(sampleOf(SnapshotOffer::type).frame);
    repeated.warnings = {"lfs_not_synced", "lfs_not_synced"};
    QCOMPARE(validate(repeated, Direction::DesktopToRunner)->field, QStringLiteral("warnings"));
}

void RunnerProtocolTest::knownWarningsAreTheThreeCodes() {
    QCOMPARE(knownWarnings(),
             (QStringList{"submodules_not_synced", "lfs_not_synced", "filtered_not_synced"}));
}

QTEST_GUILESS_MAIN(RunnerProtocolTest)
