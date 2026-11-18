#include "RunnerProtocolTest.h"

#include "protocol/Capabilities.h"
#include "protocol/FrameLimits.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace runner_protocol_test;

namespace {

template <typename T> T sample() {
    for (const Sample& s : samples()) {
        if (const T* frame = std::get_if<T>(&s.frame))
            return *frame;
    }
    qFatal("no sample");
}

QString repeat(const QString& unit, const qsizetype count) { return unit.repeated(count); }

QStringList list(const qsizetype count, const QString& item = QStringLiteral("a")) {
    QStringList result;
    for (qsizetype i = 0; i < count; ++i)
        result.append(item);
    return result;
}

// Rows: a frame and the field it must be refused for, or an empty field when it is accepted.
void row(const QString& name, const Frame& frame, const char* field = "") {
    QTest::newRow(qPrintable(name)) << frame << senderOf(frame) << QString::fromLatin1(field);
}

template <typename T, typename Change>
void spec(const QString& name, Change change, const char* field = "") {
    T frame = sample<T>();
    change(frame);
    row(name, frame, field);
}

void specRows() {
    using S = JobSpec;
    const auto argv = [](const QStringList& value) {
        return [value](S& f) { f.invocation = Invocation{value, std::nullopt}; };
    };
    const auto shell = [](const QString& value) {
        return [value](S& f) { f.invocation = Invocation{std::nullopt, value}; };
    };
    spec<S>("argv 256", argv(list(256)));
    spec<S>("argv 257", argv(list(257)), "invocation.argv");
    spec<S>("argv empty", argv({}), "invocation.argv");
    spec<S>("argv[0] empty", argv({"", "x"}), "invocation.argv");
    spec<S>("argv later entry empty", argv({"x", ""}));
    spec<S>("argv NUL", argv({"x", QString(QChar())}), "invocation.argv");
    spec<S>("shell 32 KiB", shell(repeat("a", 32 * 1024)));
    spec<S>("shell 32 KiB + 1", shell(repeat("a", 32 * 1024 + 1)), "invocation.shell");
    spec<S>("shell 32 KiB in 2-byte chars", shell(repeat(QStringLiteral("é"), 16 * 1024)));
    spec<S>("shell 32 KiB + 1 in 2-byte chars", shell(repeat(QStringLiteral("é"), 16 * 1024) + "a"),
            "invocation.shell");
    spec<S>("shell empty", shell(QString()), "invocation.shell");
    spec<S>("shell NUL", shell(QStringLiteral("a") + QChar()), "invocation.shell");

    const auto env = [](const qsizetype names, const qsizetype valueBytes) {
        return [names, valueBytes](S& f) {
            f.env.clear();
            for (qsizetype i = 0; i < names; ++i)
                f.env.insert(QStringLiteral("V%1").arg(i, 2, 10, QChar(u'0')), QString());
            if (valueBytes > 0)
                f.env.insert(QStringLiteral("V00"), repeat("x", valueBytes));
        };
    };
    spec<S>("env 64 names", env(64, 0));
    spec<S>("env 65 names", env(65, 0), "env");
    // One name "V00" (3 bytes) plus its value.
    spec<S>("env 32 KiB", env(1, 32 * 1024 - 3));
    spec<S>("env 32 KiB + 1", env(1, 32 * 1024 - 2), "env");
    const auto envName = [](const QString& name) {
        return [name](S& f) { f.env = {{name, QStringLiteral("v")}}; };
    };
    spec<S>("env name underscore", envName("_A1"));
    spec<S>("env name lower prefix", envName("ketplus_ok"));
    spec<S>("env name digit first", envName("1A"), "env");
    spec<S>("env name dash", envName("A-B"), "env");
    spec<S>("env name empty", envName(""), "env");
    spec<S>("env name non-ASCII", envName(QStringLiteral("É")), "env");
    spec<S>("env KETPLUS_ prefix", envName("KETPLUS_X"), "env");
    spec<S>("env KETPLUS_ alone", envName("KETPLUS_"), "env");
    spec<S>(
        "env value NUL",
        [](S& f) { f.env = {{QStringLiteral("A"), QStringLiteral("a") + QChar()}}; }, "env");

    const auto cwd = [](const QString& value) { return [value](S& f) { f.cwd = value; }; };
    spec<S>("cwd root", cwd(""));
    spec<S>("cwd 1024 bytes", cwd(repeat("a", 1024)));
    spec<S>("cwd 1025 bytes", cwd(repeat("a", 1025)), "cwd");
    spec<S>("cwd 1025 bytes in 2-byte chars", cwd(repeat(QStringLiteral("é"), 512) + "a"), "cwd");
    spec<S>("cwd absolute", cwd("/a"), "cwd");
    spec<S>("cwd parent", cwd(".."), "cwd");
    spec<S>("cwd parent inside", cwd("a/../b"), "cwd");
    spec<S>("cwd dotted name", cwd("a/..b/.c"));
    spec<S>("cwd empty segment", cwd("a//b"), "cwd");
    spec<S>("cwd trailing slash", cwd("a/"), "cwd");
    spec<S>("cwd dot segment", cwd("./a"), "cwd");
    spec<S>("cwd NUL", cwd(QStringLiteral("a") + QChar()), "cwd");

    const auto timeout = [](const qint64 value) { return [value](S& f) { f.timeoutMs = value; }; };
    spec<S>("timeoutMs 1 s", timeout(1000));
    spec<S>("timeoutMs below 1 s", timeout(999), "timeoutMs");
    spec<S>("timeoutMs 24 h", timeout(86400000));
    spec<S>("timeoutMs above 24 h", timeout(86400001), "timeoutMs");

    spec<S>("sha1 ids", [](S& f) { f.snapshotId = objectId('1'); });
    spec<S>(
        "sha1 with 64-digit snapshotId", [](S& f) { f.snapshotId = QString(64, u'a'); },
        "snapshotId");
    spec<S>("sha256 ids", [](S& f) {
        f.objectFormat = "sha256";
        f.snapshotId = QString(64, u'a');
        f.sourceHead = QString(64, u'b');
    });
    spec<S>(
        "sha256 with 40-digit sourceHead",
        [](S& f) {
            f.objectFormat = "sha256";
            f.snapshotId = QString(64, u'a');
        },
        "sourceHead");
    spec<S>("unknown objectFormat", [](S& f) { f.objectFormat = "md5"; }, "objectFormat");
    spec<S>("workspaceKey 31", [](S& f) { f.workspaceKey.chop(1); }, "workspaceKey");
    spec<S>("repositoryKey padded", [](S& f) { f.repositoryKey[0] = u'='; }, "repositoryKey");
}

void idRows() {
    const auto ids = [](const QString& value) {
        return [value](Hello& f) {
            f.connectionId = value;
            f.runnerId = value;
            f.os = value;
            f.arch = value;
            f.jobId = value;
        };
    };
    spec<Hello>("ids 128", ids(repeat("a", 128)));
    spec<Hello>("ids 129", ids(repeat("a", 129)), "connectionId");
    spec<Hello>("ids printable", ids(QStringLiteral("!~")));
    spec<Hello>("id with space", ids(QStringLiteral("a b")), "connectionId");
    spec<Hello>("id non-ASCII", ids(QStringLiteral("é")), "connectionId");
    spec<Hello>("runnerId 129", [](Hello& f) { f.runnerId = repeat("a", 129); }, "runnerId");
    spec<Hello>("os 129", [](Hello& f) { f.os = repeat("a", 129); }, "os");
    spec<Hello>("arch empty", [](Hello& f) { f.arch.clear(); }, "arch");
    spec<Hello>("jobId 129", [](Hello& f) { f.jobId = repeat("a", 129); }, "jobId");
    spec<Hello>("runnerVersion 64", [](Hello& f) { f.runnerVersion = repeat("1", 64); });
    spec<Hello>(
        "runnerVersion 65", [](Hello& f) { f.runnerVersion = repeat("1", 65); }, "runnerVersion");
    spec<Welcome>("daemonVersion 128", [](Welcome& f) { f.daemonVersion = repeat("1", 128); });
    spec<Welcome>(
        "daemonVersion 129", [](Welcome& f) { f.daemonVersion = repeat("1", 129); },
        "daemonVersion");
    spec<JobAccepted>("attempt 2^31-1", [](JobAccepted& f) { f.attempt = (1LL << 31) - 1; });
    spec<JobAccepted>("attempt 2^31", [](JobAccepted& f) { f.attempt = 1LL << 31; }, "attempt");
    spec<JobAccepted>("attempt 0", [](JobAccepted& f) { f.attempt = 0; }, "attempt");
    spec<Ping>("nonce 128", [](Ping& f) { f.nonce = repeat("n", 128); });
    spec<Ping>("nonce 129", [](Ping& f) { f.nonce = repeat("n", 129); }, "nonce");
    spec<Ping>("nonce empty", [](Ping& f) { f.nonce.clear(); }, "nonce");
}

void helloRows() {
    spec<Hello>("labels 64", [](Hello& f) { f.labels = list(64, repeat("l", 64)); });
    spec<Hello>("labels 65", [](Hello& f) { f.labels = list(65); }, "labels");
    spec<Hello>("label 65 chars", [](Hello& f) { f.labels = {repeat("l", 65)}; }, "labels");
    spec<Hello>("label empty", [](Hello& f) { f.labels = {QString()}; }, "labels");
    spec<Hello>("label space", [](Hello& f) { f.labels = {"a b"}; }, "labels");
    const auto capabilities = [](const QStringList& extra) {
        return [extra](Hello& f) {
            f.capabilities = QStringList{QString(capability::runnerProtocol)} + extra;
        };
    };
    spec<Hello>("capabilities 32", capabilities(list(31, repeat("c", 64))));
    spec<Hello>("capabilities 33", capabilities(list(32)), "capabilities");
    spec<Hello>("capability 65 chars", capabilities({repeat("c", 65)}), "capabilities");
    spec<Hello>("capability empty", capabilities({QString()}), "capabilities");
    spec<Welcome>("welcome capabilities 32", [](Welcome& f) { f.capabilities = list(32); });
    spec<Welcome>(
        "welcome capabilities 33", [](Welcome& f) { f.capabilities = list(33); }, "capabilities");
    const auto snapshots = [](const qsizetype count) {
        return [count](Hello& f) {
            f.snapshots.clear();
            for (qsizetype i = 0; i < count; ++i)
                f.snapshots.insert(QStringLiteral("%1").arg(i, 32, 10, QChar(u'0')), objectId());
        };
    };
    spec<Hello>("snapshots 256", snapshots(256));
    spec<Hello>("snapshots 257", snapshots(257), "snapshots");
    spec<Hello>("snapshots sha256 id",
                [](Hello& f) { f.snapshots = {{key(), QString(64, u'a')}}; });
    spec<Hello>(
        "snapshots bad key", [](Hello& f) { f.snapshots = {{"short", objectId()}}; }, "snapshots");
    spec<Hello>("snapshots bad id", [](Hello& f) { f.snapshots = {{key(), "abc"}}; }, "snapshots");
}

void snapshotRows() {
    using O = SnapshotOffer;
    spec<O>("packBytes 1", [](O& f) { f.packBytes = 1; });
    spec<O>("packBytes 0", [](O& f) { f.packBytes = 0; }, "packBytes");
    spec<O>("packBytes 2^53-1", [](O& f) { f.packBytes = (1LL << 53) - 1; });
    spec<O>("packBytes 2^53", [](O& f) { f.packBytes = 1LL << 53; }, "packBytes");
    spec<O>("chunkBytes 4 MiB - 1", [](O& f) { f.chunkBytes = 4194303; }, "chunkBytes");
    spec<O>("chunkBytes 4 MiB + 1", [](O& f) { f.chunkBytes = 4194305; }, "chunkBytes");
    spec<O>("baseId sha256", [](O& f) { f.baseId = QString(64, u'0'); });
    spec<O>("baseId 41", [](O& f) { f.baseId = QString(41, u'0'); }, "baseId");
    spec<O>("all three warnings", [](O& f) { f.warnings = knownWarnings(); });
    spec<O>("unknown warning", [](O& f) { f.warnings = {"lfs_not_sync"}; }, "warnings");
    spec<O>("packSha256 63", [](O& f) { f.packSha256.chop(1); }, "packSha256");
    using C = SnapshotChunk;
    spec<C>("chunk bytes 1", [](C& f) { f.bytes = 1; });
    spec<C>("chunk bytes 0", [](C& f) { f.bytes = 0; }, "bytes");
    spec<C>("chunk bytes 4 MiB + 1", [](C& f) { f.bytes = 4194305; }, "bytes");
    spec<C>("transferId 15", [](C& f) { f.transferId.chop(1); }, "transferId");
    spec<C>("transferId upper", [](C& f) { f.transferId = "0123456789ABCDEF"; }, "transferId");
    spec<C>("offset 2^53-1", [](C& f) { f.offset = (1LL << 53) - 1; });
    spec<C>("offset 2^53", [](C& f) { f.offset = 1LL << 53; }, "offset");
    spec<C>("offset -1", [](C& f) { f.offset = -1; }, "offset");
    using W = SnapshotWant;
    spec<W>("want fromOffset -1", [](W& f) { f.fromOffset = -1; }, "fromOffset");
    const auto want = [](const char* state, const qint64 fromOffset) {
        return [state, fromOffset](W& f) {
            f.state = QString::fromLatin1(state);
            f.fromOffset = fromOffset;
        };
    };
    spec<W>("want have from 0", want("have", 0));
    spec<W>("want have from 1", want("have", 1), "fromOffset");
    spec<W>("want need from 1", want("need", 1));
    spec<JobResume>(
        "outputOffset 2^53", [](JobResume& f) { f.outputOffset = 1LL << 53; }, "outputOffset");
    spec<JobOutputAck>("epoch 2^53", [](JobOutputAck& f) { f.epoch = 1LL << 53; }, "epoch");
}

void codeRows() {
    spec<JobRejected>("code 64", [](JobRejected& f) { f.code = repeat("c", 64); });
    spec<JobRejected>("code 65", [](JobRejected& f) { f.code = repeat("c", 65); }, "code");
    spec<SnapshotError>(
        "snapshot code 65", [](SnapshotError& f) { f.code = repeat("c", 65); }, "code");
    spec<JobResult>("result reason 64", [](JobResult& f) { f.reason = repeat("r", 64); });
    spec<JobResult>("result reason 65", [](JobResult& f) { f.reason = repeat("r", 65); }, "reason");
    spec<JobResult>("result reason empty", [](JobResult& f) { f.reason.clear(); });
    spec<JobResult>("result reason space", [](JobResult& f) { f.reason = "a b"; }, "reason");
    spec<JobResult>("result reason upper", [](JobResult& f) { f.reason = "A"; }, "reason");
    spec<Close>("close reason 64", [](Close& f) { f.reason = repeat(" ", 64); });
    spec<Close>("close reason 65", [](Close& f) { f.reason = repeat("r", 65); }, "reason");
    spec<Close>("close reason tab", [](Close& f) { f.reason = "\t"; }, "reason");
    spec<Close>(
        "close reason non-ASCII", [](Close& f) { f.reason = QStringLiteral("é"); }, "reason");
    for (const char* reason : {"user", "deadline", "session_closed"})
        spec<JobCancel>(QStringLiteral("cancel %1").arg(reason),
                        [reason](JobCancel& f) { f.reason = reason; });
    spec<JobCancel>("cancel other", [](JobCancel& f) { f.reason = "timeout"; }, "reason");
}

void resultRows() {
    using R = JobResult;
    spec<R>("exitCode -2^31", [](R& f) { f.exitCode = -(1LL << 31); });
    spec<R>("exitCode -2^31-1", [](R& f) { f.exitCode = -(1LL << 31) - 1; }, "exitCode");
    spec<R>("exitCode 2^31-1", [](R& f) { f.exitCode = (1LL << 31) - 1; });
    spec<R>("exitCode 2^31", [](R& f) { f.exitCode = 1LL << 31; }, "exitCode");
    const auto signal = [](const qint64 value) {
        return [value](R& f) {
            f.exitCode = std::nullopt;
            f.signal = value;
        };
    };
    spec<R>("signal 1", signal(1));
    spec<R>("signal 0", signal(0), "signal");
    spec<R>("signal 64", signal(64));
    spec<R>("signal 65", signal(65), "signal");
    spec<R>("exitCode and signal", [](R& f) { f.signal = 9; }, "signal");
    spec<R>("outputBytes 2^53", [](R& f) { f.outputBytes = 1LL << 53; }, "outputBytes");
    spec<R>("fractional time", [](R& f) { f.endedAt = "2026-11-18T01:02:03.123456789Z"; });
    spec<R>("offset time", [](R& f) { f.endedAt = "2026-11-18T01:02:03+07:00"; }, "endedAt");
    spec<R>("impossible date", [](R& f) { f.startedAt = "2026-02-30T01:02:03Z"; }, "startedAt");
    spec<R>("unverified cleanup", [](R& f) { f.cleanupStatus = "unverified"; });
    spec<R>("unknown cleanup", [](R& f) { f.cleanupStatus = "maybe"; }, "cleanupStatus");
    using O = JobOutput;
    spec<O>("output 32 KiB", [](O& f) { f.data = QByteArray(32 * 1024, 'x'); });
    spec<O>("output 32 KiB + 1", [](O& f) { f.data = QByteArray(32 * 1024 + 1, 'x'); }, "data");
    spec<O>("output 1 byte", [](O& f) { f.data = QByteArray(1, '\0'); });
    spec<O>("output empty", [](O& f) { f.data.clear(); }, "data");
    using G = JobOutputGap;
    spec<G>("gap of one byte", [](G& f) { f.toOffset = f.fromOffset + 1; });
    spec<G>("gap empty", [](G& f) { f.toOffset = f.fromOffset; }, "toOffset");
}

} // namespace

void RunnerProtocolTest::frameSizeLimits_data() {
    QTest::addColumn<Frame>("frame");
    QTest::addColumn<Direction>("direction");
    for (const Sample& s : samples())
        QTest::newRow(frameType(s.frame).data()) << s.frame << s.direction;
}

void RunnerProtocolTest::frameSizeLimits() {
    QFETCH(Frame, frame);
    QFETCH(Direction, direction);
    const Route* route = routeFor(frameType(frame));
    // Grow the frame with an ignored field to exactly its limit, then one byte more.
    QJsonObject object = detail::toObject(frame);
    object.insert("pad", QString());
    const qsizetype base = compact(object).size();
    QVERIFY(base < route->maxBytes);
    object.insert("pad", QString(route->maxBytes - base, u'p'));
    QByteArray message = compact(object);
    QCOMPARE(message.size(), route->maxBytes);
    QVERIFY2(decode(message, route->channel, direction).ok(), frameType(frame).data());

    object.insert("pad", QString(route->maxBytes - base + 1, u'p'));
    message = compact(object);
    const DecodeResult result = decode(message, route->channel, direction);
    QVERIFY(!result.ok());
    QCOMPARE(result.error.kind, FrameError::Kind::Oversize);
    QCOMPARE(result.error.field, QString());
}

void RunnerProtocolTest::anyMessageAboveTheLargestLimitIsOversize() {
    // Refused before parsing, whatever the content.
    const QByteArray garbage(limits::maxFrameBytes + 1, '{');
    const DecodeResult result = decode(garbage, Channel::Control, Direction::RunnerToDesktop);
    QCOMPARE(result.error.kind, FrameError::Kind::Oversize);
    QCOMPARE(result.error.type, QString());
}

void RunnerProtocolTest::encodeRefusesAnOversizeFrame() {
    // A 1 KiB frame with a valid but long nonce, and a 64 KiB one at the edge.
    JobOutputAck ack = sample<JobOutputAck>();
    ack.jobId = QString(128, u'j');
    QVERIFY(encode(ack, Direction::DesktopToRunner).ok());

    JobSpec spec = sample<JobSpec>();
    spec.invocation = Invocation{QStringList(256, QString(255, u'a')), std::nullopt};
    const EncodeResult encoded = encode(spec, Direction::DesktopToRunner);
    QVERIFY(!encoded.ok());
    QCOMPARE(encoded.error->kind, FrameError::Kind::Oversize);
    QCOMPARE(encoded.error->type, QStringLiteral("job.spec"));
    QVERIFY(encoded.message.isEmpty());
}

void RunnerProtocolTest::packLimitIsTheCallersCheck() {
    QCOMPARE(limits::maxPackBytes, qint64(4294967296));
    QVERIFY(!limits::packTooLarge(4294967296));
    QVERIFY(limits::packTooLarge(4294967297));
    for (const qint64 bytes : {qint64(4294967296), qint64(4294967297)}) {
        SnapshotOffer offer = sample<SnapshotOffer>();
        offer.packBytes = bytes;
        const EncodeResult encoded = encode(offer, Direction::DesktopToRunner);
        QVERIFY(encoded.ok());
        const DecodeResult decoded =
            decode(encoded.message, Channel::Control, Direction::DesktopToRunner);
        QVERIFY(decoded.ok());
        QCOMPARE(std::get<SnapshotOffer>(*decoded.frame).packBytes, bytes);
    }
}

void RunnerProtocolTest::fieldLimits_data() {
    QTest::addColumn<Frame>("frame");
    QTest::addColumn<Direction>("direction");
    QTest::addColumn<QString>("field");
    specRows();
    idRows();
    helloRows();
    snapshotRows();
    codeRows();
    resultRows();
}

void RunnerProtocolTest::fieldLimits() {
    QFETCH(Frame, frame);
    QFETCH(Direction, direction);
    QFETCH(QString, field);
    const Route* route = routeFor(frameType(frame));
    const QByteArray raw = compact(detail::toObject(frame));
    QVERIFY2(raw.size() <= route->maxBytes, "row must fit its frame");
    const std::optional<FrameError> checked = validate(frame, direction);
    const DecodeResult decoded = decode(raw, route->channel, direction);
    if (field.isEmpty()) {
        QVERIFY2(!checked, qPrintable(checked ? checked->field : QString()));
        QVERIFY2(decoded.ok(), qPrintable(decoded.error.field));
        QVERIFY(*decoded.frame == frame);
        return;
    }
    // The sender and the receiver refuse it for the same field.
    QVERIFY(checked.has_value());
    QCOMPARE(checked->kind, FrameError::Kind::InvalidField);
    QCOMPARE(checked->field, field);
    QVERIFY(!encode(frame, direction).ok());
    QVERIFY(!decoded.ok());
    QCOMPARE(decoded.error.kind, FrameError::Kind::InvalidField);
    QCOMPARE(decoded.error.field, field);
}
