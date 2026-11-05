#include "BuildProvenance.h"
#include "MetricsArtifact.h"
#include "ProjectionChecks.h"
#include "WorkloadAdapter.h"
#include "protocol/DaemonProtocol.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QSysInfo>

using namespace ketplus::daemon;

class CharacterizationTest final : public QObject {
    Q_OBJECT
    QJsonArray measurements_;
    QJsonObject manifest_;
    QSet<QString> completedTests_;

    QJsonArray fixture(const QString& name) {
        QFile file(qEnvironmentVariable("R03_FIXTURES") + "/" + name + ".events.jsonl");
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error("missing materialized fixture");
        const auto bytes = file.readAll();
        QJsonObject specification;
        for (const auto& value : manifest_.value("scenarios").toArray())
            if (value.toObject().value("name") == name)
                specification = value.toObject();
        const auto integrity = specification.value("exactJsonl").toObject();
        if (integrity.value("bytes").toInteger() != bytes.size() ||
            integrity.value("sha256").toString() !=
                QString::fromLatin1(
                    QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()))
            throw std::runtime_error("fixture integrity mismatch");
        QJsonArray events;
        for (const auto& line : bytes.split('\n')) {
            if (line.isEmpty())
                continue;
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject())
                throw std::runtime_error("invalid event JSONL");
            events.append(document.object());
        }
        if (events.size() != integrity.value("count").toInteger())
            throw std::runtime_error("fixture count mismatch");
        return events;
    }

  private slots:
    void cleanup() {
        if (!QTest::currentTestFailed())
            completedTests_.insert(QString::fromLatin1(QTest::currentTestFunction()));
    }
    void initTestCase() {
        QFile file(qEnvironmentVariable("R03_FIXTURES") + "/manifest.json");
        QVERIFY2(file.open(QIODevice::ReadOnly),
                 "Set R03_FIXTURES to a materialized v3 workload directory");
        manifest_ = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(manifest_.value("version").toInt(), 3);
        QVERIFY(!manifest_.value("seed").toString().isEmpty());
    }

    void cloudLifecycle() { r03::productionProjectionChecks(); }

    void workloadCommandAdapter() {
        const auto events = fixture("command-output-cloud-strip-marker");
        const auto mapped = r03::adaptCommandScenario(events);
        QCOMPARE(mapped.size(), 3); // Aggregate is folded into completed provider item.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, r03::adapt(events.at(2).toObject()));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 r03::adapt(QJsonObject{{"event_type", "unknown"}}));
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            r03::adapt(fixture("rich-markdown-media-command").at(2).toObject()));
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("commands.sqlite"));
        QVERIFY(
            journal.upsertSession({"commands", directory.path(), "synthetic", {}, {}, "ready"}));
        for (const auto& e : mapped) {
            QVERIFY(journal.appendEvent("commands", 1, e.type, e.payload) > 0);
            const auto projected = projectEventPayloadForCloud(e.type, e.payload);
            const auto bytes = QJsonDocument(projected).toJson(QJsonDocument::Compact);
            QVERIFY(!bytes.contains("KETPLUS_COMMAND_OUTPUT_SHOULD_NOT_REACH_CLOUD"));
            if (e.type != "command.output_delta") {
                QCOMPARE(projected.value("command"), e.payload.value("command"));
                QCOMPARE(projected.value("item").toObject().value("status"),
                         e.payload.value("item").toObject().value("status"));
            }
        }
        const auto raw =
            QJsonDocument(journal.eventsAfter("commands", 1, 0)).toJson(QJsonDocument::Compact);
        QVERIFY(raw.contains("KETPLUS_COMMAND_OUTPUT_SHOULD_NOT_REACH_CLOUD_AGGREGATED"));
        QVERIFY(raw.contains("line 1"));
    }

    void pagingGenerationDuplicatesAndReopen() {
        QTemporaryDir directory;
        const auto path = directory.filePath("paging.sqlite");
        {
            LocalJournal journal(path);
            QVERIFY(
                journal.upsertSession({"paging", directory.path(), "synthetic", {}, {}, "ready"}));
            for (int turn = 1; turn <= 5; ++turn) {
                QVERIFY(journal.appendEvent("paging", 1, "user.message",
                                            {{"text", QString::number(turn)}}) > 0);
                for (const auto& id : {QString("a"), QString("tool"), QString("b")}) {
                    const QJsonObject item{{"id", id + QString::number(turn)},
                                           {"type", id == "tool" ? "mcpToolCall" : "agentMessage"},
                                           {"text", "synthetic"}};
                    QVERIFY(journal.appendEvent("paging", 1, "item.completed", {{"item", item}}) >
                            0);
                }
            }
            const auto latest = journal.transcriptPage("paging", 1, 0, 3);
            QCOMPARE(latest.snapshotCursor, 20);
            QCOMPARE(latest.events.size(), 12);
            QCOMPARE(latest.earliestTurnSequence, 3);
            QVERIFY(latest.hasMoreBefore);
            QVERIFY(!latest.truncated);
            const auto older = journal.transcriptPage("paging", 1, latest.earliestTurnSequence, 3);
            QCOMPARE(older.events.size(), 8);
            QCOMPARE(older.events.first().toObject().value("cursor").toInteger(), 1);
            QCOMPARE(older.events.last().toObject().value("cursor").toInteger(), 8);
            // appendEvent allocates a cursor; it does not deduplicate identical payloads.
            QCOMPARE(journal.appendEvent("paging", 1, "item.completed", {{"item_id", "duplicate"}}),
                     21);
            QCOMPARE(journal.appendEvent("paging", 1, "item.completed", {{"item_id", "duplicate"}}),
                     22);
            QVERIFY(journal.storeCommandResult("retry", {{"value", 1}}));
            QVERIFY(journal.storeCommandResult("retry", {{"value", 2}}));
            QCOMPARE(journal.commandResult("retry").value("value").toInt(), 1);
            QVERIFY(journal.upsertSession(
                {"paging", directory.path(), "synthetic", {}, {}, "ready", 2}));
            QCOMPARE(journal.appendEvent("paging", 2, "user.message", {{"text", "generation two"}}),
                     1);
            QCOMPARE(journal.appendEvent("paging", 1, "user.message", {{"text", "stale"}}), -1);
            QCOMPARE(journal.eventsAfter("paging", 1, 0).size(), 22);
            QCOMPARE(journal.eventsAfter("paging", 2, 0).size(), 1);
        }
        LocalJournal reopened(path);
        QVERIFY(reopened.isOpen());
        QCOMPARE(reopened.eventsAfter("paging", 1, 0).size(), 22);
        QCOMPARE(reopened.transcriptPage("paging", 2, 0, 3).events.size(), 1);
        QVERIFY(reopened.acknowledgeCursor("synthetic-reader", "paging", 2, 1));
        QVERIFY(reopened.eventsAfter("paging", 2, 0).isEmpty());
        QCOMPARE(reopened.compactedEventsAfter("paging", 2, 0, 500).events.size(), 1);
    }

    void productionWorkloadSamples() {
        for (const auto& name :
             {QString("items-1k"), QString("items-10k"), QString("giant-turn")}) {
            const auto events = fixture(name);
            if (name == "giant-turn") {
                QVERIFY(events.size() > 500);
                QVERIFY(events.last()
                            .toObject()
                            .value("payload")
                            .toObject()
                            .value("item")
                            .toObject()
                            .value("text")
                            .toString()
                            .toUtf8()
                            .size() >= 8 * 1024 * 1024);
            }
            for (int sample = 0; sample < 3; ++sample) {
                QTemporaryDir directory;
                QVERIFY(directory.isValid());
                const auto path = directory.filePath("workload.sqlite");
                QJsonObject row{
                    {"scenario", name}, {"sample", sample}, {"inputEvents", events.size()}};
                qint64 accepted = 0, rejected = 0, mappedBytes = 0;
                {
                    LocalJournal journal(path);
                    QVERIFY(journal.isOpen());
                    QVERIFY(journal.upsertSession(
                        {"selected", directory.path(), "synthetic", {}, {}, "ready"}));
                    if (name == "giant-turn") {
                        // Workload lacks a user event; declare this prelude so v1 creates turn 1.
                        QCOMPARE(journal.appendEvent("selected", 1, "user.message",
                                                     {{"text", "giant prelude"}}),
                                 1);
                    }
                    QList<r03::NormalizedEvent> mapped;
                    for (const auto& value : events) {
                        auto e = r03::adapt(value.toObject());
                        mappedBytes +=
                            QJsonDocument(e.payload).toJson(QJsonDocument::Compact).size();
                        mapped.append(e);
                    }
                    QElapsedTimer timer;
                    timer.start();
                    for (const auto& e : mapped) {
                        QString error;
                        const auto cursor =
                            journal.appendEvent("selected", 1, e.type, e.payload, &error);
                        if (cursor < 0) {
                            ++rejected;
                            QVERIFY(name == "giant-turn" && e.type == "item.completed");
                            QVERIFY(error.contains("limit"));
                        } else
                            ++accepted;
                    }
                    row.insert("appendElapsedNs", timer.nsecsElapsed());
                    timer.restart();
                    const auto latest = journal.transcriptPage("selected", 1, 0, 3);
                    row.insert("newestPageElapsedNs", timer.nsecsElapsed());
                    row.insert("newestPageEvents", latest.events.size());
                    row.insert("newestPageSerializedBytes",
                               QJsonDocument(latest.events).toJson(QJsonDocument::Compact).size());
                    row.insert("snapshotCursor", latest.snapshotCursor);
                    row.insert("truncated", latest.truncated);
                    row.insert("hasMoreBefore", latest.hasMoreBefore);
                    QCOMPARE(rejected, 0);
                    if (name == "giant-turn") {
                        QCOMPARE(latest.events.size(), 3);
                        QVERIFY(!latest.truncated);
                        QVERIFY(!latest.hasMoreBefore);
                        QVERIFY(journal.transcriptPage("selected", 1, 1, 3).events.isEmpty());
                        const auto reference = latest.events.last()
                                                   .toObject()
                                                   .value("payload")
                                                   .toObject()
                                                   .value("large_event")
                                                   .toObject();
                        QCOMPARE(reference.value("version").toInt(), 1);
                        QCOMPARE(latest.snapshotCursor, reference.value("revision").toInteger());
                        QVERIFY(latest.snapshotCursor > accepted);
                        QByteArray restored;
                        qint64 offset = 0;
                        while (offset < reference.value("total_bytes").toInteger()) {
                            const auto fragment = journal.largeEventFragment(
                                "selected", 1, reference.value("transfer_id").toString(), offset,
                                128 * 1024);
                            QVERIFY2(fragment.errorCode.isEmpty(),
                                     qPrintable(fragment.errorMessage));
                            QVERIFY(fragment.data.size() <= 128 * 1024);
                            restored.append(fragment.data);
                            QVERIFY(fragment.nextOffset > offset);
                            offset = fragment.nextOffset;
                        }
                        QCOMPARE(
                            restored,
                            QJsonDocument(mapped.last().payload).toJson(QJsonDocument::Compact));
                    } else {
                        QCOMPARE(latest.events.size(), 6);
                        QVERIFY(latest.hasMoreBefore);
                        QVERIFY(!latest.truncated);
                        QCOMPARE(accepted, events.size());
                    }
                    qint64 cursor = 0, count = 0, serializedBytes = 0, projectedBytes = 0,
                           projectionNs = 0;
                    timer.restart();
                    while (true) {
                        const auto page = journal.compactedEventsAfter("selected", 1, cursor, 500);
                        if (page.events.isEmpty())
                            break;
                        QVERIFY(page.nextCursor > cursor);
                        count += page.events.size();
                        serializedBytes +=
                            QJsonDocument(page.events).toJson(QJsonDocument::Compact).size();
                        QElapsedTimer projectionTimer;
                        projectionTimer.start();
                        for (const auto& value : page.events) {
                            const auto e = value.toObject();
                            projectedBytes += QJsonDocument(projectEventPayloadForCloud(
                                                                e.value("event_type").toString(),
                                                                e.value("payload").toObject()))
                                                  .toJson(QJsonDocument::Compact)
                                                  .size();
                        }
                        projectionNs += projectionTimer.nsecsElapsed();
                        cursor = page.nextCursor;
                    }
                    row.insert("compactReplayAndProjectionElapsedNs", timer.nsecsElapsed());
                    row.insert("projectionAndSerializationElapsedNs", projectionNs);
                    row.insert("compactReplayEvents", count);
                    row.insert("compactReplaySerializedArrayBytes", serializedBytes);
                    row.insert("projectedPayloadBytes", projectedBytes);
                    QCOMPARE(count, name == "giant-turn" ? 3 : accepted);
                }
                QElapsedTimer reopen;
                reopen.start();
                LocalJournal journal(path);
                QVERIFY(journal.isOpen());
                row.insert("reopenElapsedNs", reopen.nsecsElapsed());
                row.insert("acceptedWorkloadEvents", accepted);
                row.insert("rejectedWorkloadEvents", rejected);
                row.insert("mappedPayloadBytes", mappedBytes);
                measurements_.append(row);
            }
        }
    }

  public:
    int publish(int testExitCode) {
        const auto output = qEnvironmentVariable("R03_METRICS");
        if (output.isEmpty())
            return testExitCode;
        const auto status = r03::metricsStatus(testExitCode, completedTests_, measurements_);
        auto completed = completedTests_.values();
        completed.sort();
        const QJsonObject report{
            {"status", status},
            {"baselineValid", status == "complete"},
            {"testExitCode", testExitCode},
            {"completedTests", QJsonArray::fromStringList(completed)},
            {"publishedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"buildSha", R03_BUILD_SHA},
            {"sourceDirty", bool(R03_SOURCE_DIRTY)},
            {"sourceDigest", R03_SOURCE_DIGEST},
            {"provenanceScope", "build-time digest of production/test source and headers; git "
                                "dirty flag covers worktree"},
            {"compiler", R03_COMPILER},
            {"qt", qVersion()},
            {"buildType", "Release"},
            {"platform", QSysInfo::prettyProductName()},
            {"architecture", QSysInfo::currentCpuArchitecture()},
            {"fixtureManifest", manifest_},
            {"samplesPerScenario", 3},
            {"rawMeasurements", measurements_},
            {"nativeViewportFrameLayoutWebView", "not_measured"},
            {"rss", "not_measured"},
            {"encryptionUpload", "not_verified"},
            {"percentiles", "not_estimated"}};
        QString error;
        if (!r03::publishMetrics(output, report, &error)) {
            qCritical().noquote() << "Metrics publish failed:" << error;
            return 1;
        }
        return status == "complete" ? 0 : 1;
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    CharacterizationTest test;
    int result = 1;
    try {
        result = QTest::qExec(&test, argc, argv);
    } catch (const std::exception& error) {
        qCritical().noquote() << "Characterization failed:" << error.what();
    }
    return test.publish(result);
}
#include "CharacterizationTest.moc"
