#include "RunnerAppTest.h"

#include "delivery/DeliveryRecords.h"
#include "state/AcceptedJobsFile.h"
#include "state/JobJournalFile.h"
#include "state/PinTrust.h"
#include "state/RunnerState.h"
#include "state/StateDirectory.h"

#include <QJsonArray>

#include <QFile>
#include <QTest>

#include <sys/stat.h>

namespace runner_app_test {

using namespace ketplus::runner::app;

namespace {

std::optional<StateDirectory> openState(const TestHost& host) {
    QString error;
    std::optional<StateDirectory> directory = StateDirectory::open(host.stateDir(), &error);
    if (!directory)
        qWarning("%s", qPrintable(error));
    return directory;
}

} // namespace

void RunnerAppDeliveryFilesTest::acceptedJobsSurviveARestart() {
    TestHost host;
    QDateTime now = host.clock;
    const auto clock = [&] { return now; };
    QString error;
    {
        std::optional<StateDirectory> directory = openState(host);
        QVERIFY(directory);
        std::optional<AcceptedJobsFile> accepted =
            AcceptedJobsFile::load(*directory, clock, &error);
        QVERIFY2(accepted, qPrintable(error));
        QVERIFY(!accepted->contains(QStringLiteral("job-1"), 1));
        QVERIFY(accepted->add(QStringLiteral("job-1"), 1));
        QVERIFY(accepted->add(QStringLiteral("job-1"), 1)); // once
        QCOMPARE(accepted->size(), 1);
    }
    // A new process: the record is still there, for this attempt only.
    std::optional<StateDirectory> directory = openState(host);
    QVERIFY(directory);
    std::optional<AcceptedJobsFile> accepted = AcceptedJobsFile::load(*directory, clock, &error);
    QVERIFY2(accepted, qPrintable(error));
    QVERIFY(accepted->contains(QStringLiteral("job-1"), 1));
    QVERIFY(!accepted->contains(QStringLiteral("job-1"), 2));
    const QByteArray file = directory->read(QStringLiteral("accepted.json")).data;
    QVERIFY(file.contains("\"jobId\":\"job-1\""));
    QVERIFY(!file.contains("lease")); // the job and attempt only
}

void RunnerAppDeliveryFilesTest::acceptedJobsExpireAfterSevenDays() {
    TestHost host;
    QDateTime now = host.clock;
    const auto clock = [&] { return now; };
    QString error;
    std::optional<StateDirectory> directory = openState(host);
    QVERIFY(directory);
    std::optional<AcceptedJobsFile> accepted = AcceptedJobsFile::load(*directory, clock, &error);
    QVERIFY(accepted->add(QStringLiteral("job-1"), 1));
    now = now.addDays(1);
    QVERIFY(accepted->add(QStringLiteral("job-2"), 1));
    now = host.clock.addDays(7).addSecs(-1);
    QVERIFY(accepted->contains(QStringLiteral("job-1"), 1)); // the last second of the 7 days
    now = host.clock.addDays(7);
    QVERIFY(!accepted->contains(QStringLiteral("job-1"), 1));
    QVERIFY(accepted->contains(QStringLiteral("job-2"), 1));
    // The expired record leaves the file at the next write and is not loaded again.
    QVERIFY(accepted->add(QStringLiteral("job-3"), 1));
    QVERIFY(!directory->read(QStringLiteral("accepted.json")).data.contains("job-1"));
    std::optional<AcceptedJobsFile> reloaded = AcceptedJobsFile::load(*directory, clock, &error);
    QCOMPARE(reloaded->size(), 2);
}

void RunnerAppDeliveryFilesTest::corruptAcceptedJobsAreReported_data() {
    QTest::addColumn<QByteArray>("content");
    QTest::newRow("not json") << QByteArray("{");
    QTest::newRow("other version") << QByteArray(R"({"version":2,"jobs":[]})");
    QTest::newRow("no list") << QByteArray(R"({"version":1})");
    QTest::newRow("bad job id") << QByteArray(
        R"({"version":1,"jobs":[{"jobId":"a b","attempt":1,"acceptedAt":"2026-11-18T09:00:00Z"}]})");
    QTest::newRow("no attempt") << QByteArray(
        R"({"version":1,"jobs":[{"jobId":"j","acceptedAt":"2026-11-18T09:00:00Z"}]})");
    QTest::newRow("bad time") << QByteArray(
        R"({"version":1,"jobs":[{"jobId":"j","attempt":1,"acceptedAt":"soon"}]})");
}

void RunnerAppDeliveryFilesTest::corruptAcceptedJobsAreReported() {
    QFETCH(QByteArray, content);
    TestHost host;
    std::optional<StateDirectory> directory = openState(host);
    QVERIFY(directory);
    QString error;
    QVERIFY(directory->write(QStringLiteral("accepted.json"), content, &error));
    const auto clock = [&] { return host.clock; };
    QVERIFY(!AcceptedJobsFile::load(*directory, clock, &error));
    QVERIFY(error.contains(QStringLiteral("accepted.json is corrupt")));
    QCOMPARE(directory->read(QStringLiteral("accepted.json")).data, content); // left as it is
}

void RunnerAppDeliveryFilesTest::failedAcceptedWriteKeepsNothing() {
    TestHost host;
    std::optional<StateDirectory> directory = openState(host);
    QVERIFY(directory);
    QString error;
    const auto clock = [&] { return host.clock; };
    std::optional<AcceptedJobsFile> accepted = AcceptedJobsFile::load(*directory, clock, &error);
    const QByteArray path = QFile::encodeName(host.stateDir());
    QCOMPARE(::chmod(path.constData(), 0500), 0); // no new file can be made
    const bool added = accepted->add(QStringLiteral("job-1"), 1);
    QCOMPARE(::chmod(path.constData(), 0700), 0);
    QVERIFY(!added);
    QVERIFY(accepted->error().contains(QStringLiteral("accepted.json")));
    QVERIFY(!accepted->contains(QStringLiteral("job-1"), 1));
}

void RunnerAppDeliveryFilesTest::journalFileHoldsTheRecordOnly() {
    TestHost host;
    host.registerRunner();
    std::optional<StateDirectory> directory = openState(host);
    QVERIFY(directory);
    JobJournalFile journal(*directory);
    JobJournalFile::Loaded loaded = journal.load();
    QVERIFY(loaded.ok);
    QVERIFY(!loaded.record); // no job

    const QJsonObject body{
        {QStringLiteral("jobId"), QStringLiteral("job-1")},
        {QStringLiteral("attempt"), 2},
        {QStringLiteral("leaseId"), QStringLiteral("lease-1")},
        {QStringLiteral("leaseExpiresAt"), QStringLiteral("2026-11-18T09:05:00Z")},
        {QStringLiteral("requester"), QJsonObject{{QStringLiteral("workspaceId"), "ws-1"},
                                                  {QStringLiteral("machineId"), "desktop-1"}}},
        {QStringLiteral("runsOn"), QJsonArray{QStringLiteral("gpu")}}};
    ketplus::runner::delivery::JournalRecord record;
    record.step = ketplus::runner::delivery::JournalRecord::Step::Closing;
    record.lease = *ketplus::runner::delivery::leaseFromReply(body);
    record.startedAt = QStringLiteral("2026-11-18T09:00:00Z");
    record.outcome = QStringLiteral("succeeded");
    record.reason = QStringLiteral("exited");
    QVERIFY(journal.write(record));

    loaded = journal.load();
    QVERIFY2(loaded.ok, qPrintable(loaded.error));
    QCOMPARE(loaded.record->lease.leaseId, QStringLiteral("lease-1"));
    QCOMPARE(loaded.record->lease.attempt, 2);
    QCOMPARE(loaded.record->outcome, QStringLiteral("succeeded"));
    // Ids, labels, times and codes: no secret of the runner, no command, environment or output.
    const QByteArray file = directory->read(QStringLiteral("journal.json")).data;
    for (const QString& secret : host.relay->secrets())
        QVERIFY(!file.contains(secret.toUtf8()));
    for (const char* word : {"argv", "env", "token", "credential", "output", "cwd"})
        QVERIFY2(!file.contains(word), word);
    QVERIFY(journal.clear());
    QVERIFY(!directory->exists(QStringLiteral("journal.json")));
    QVERIFY(journal.clear()); // clearing twice is fine
}

void RunnerAppDeliveryFilesTest::corruptJournalIsReported() {
    for (const QByteArray& content :
         {QByteArray("{"), QByteArray(R"({"version":1,"step":"leased","jobId":"j"})"),
          QByteArray(R"({"version":9,"step":"acquiring","acquireId":"a"})")}) {
        TestHost host;
        std::optional<StateDirectory> directory = openState(host);
        QVERIFY(directory);
        QString error;
        QVERIFY(directory->write(QStringLiteral("journal.json"), content, &error));
        const JobJournalFile::Loaded loaded = JobJournalFile(*directory).load();
        QVERIFY(!loaded.ok);
        QVERIFY(loaded.error.contains(QStringLiteral("journal.json is corrupt")));
        QCOMPARE(directory->read(QStringLiteral("journal.json")).data, content);
    }
}

void RunnerAppDeliveryFilesTest::activePinsAreTrusted() {
    TestHost host;
    host.registerRunner(); // paired and approved by desktop-1
    std::optional<StateDirectory> directory = openState(host);
    QVERIFY(directory);
    const RunnerState state(*directory);
    const PinTrust trust(state);
    QVERIFY(trust.trusted(QStringLiteral("desktop-1")));
    QVERIFY(!trust.trusted(QStringLiteral("desktop-2")));
    QString error;
    QVERIFY(state.removePin(QStringLiteral("desktop-1"), &error));
    QVERIFY(!trust.trusted(QStringLiteral("desktop-1"))); // read from disk each time
}

} // namespace runner_app_test
