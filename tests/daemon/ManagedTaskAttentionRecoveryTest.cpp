#include "FakeCodex.h"
#include "ManagedTaskIntentLookup.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "runtime/executor/NativeJobLease.h"
#include "runtime/executor/NativeProcessIdentity.h"
#include "storage/BackgroundTaskLedger.h"
#include "storage/BackgroundTaskOutput.h"
#include "storage/LocalJournal.h"
#ifdef Q_OS_MACOS
#include "ManagedTaskAttentionCleanup.h"
#endif

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtTest>
#include <csignal>
#include <memory>

using namespace ketplus::daemon;

namespace {
QByteArray bytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.read(256) : QByteArray{};
}
QJsonObject command(SessionHost& host, const QString& type, const QJsonObject& payload = {}) {
    auto result = std::make_shared<QJsonObject>();
    host.handleCommandDeferred({{"message_id", QUuid::createUuid().toString()},
                                {"message_type", type},
                                {"session_id", "owner"},
                                {"session_generation", 1},
                                {"payload", payload}},
                               "attention-test-user",
                               [result](const QJsonObject& reply) { *result = reply; });
    QElapsedTimer timer;
    timer.start();
    while (result->isEmpty() && timer.elapsed() < 6000)
        QTest::qWait(10);
    return *result;
}
QJsonObject task(SessionHost& host, const QString& id, QJsonObject* snapshot = nullptr) {
    const auto result =
        command(host, "session.task.get", {{"task_id", id}}).value("task").toObject();
    if (snapshot)
        *snapshot = result;
    return result;
}
bool seed(LocalJournal& journal, const QString& cwd, qint64 generation = 1) {
    StoredSession owner;
    owner.id = "owner";
    owner.generation = generation;
    owner.worktreePath = cwd;
    owner.adapter = "codex";
    owner.state = "ready";
    owner.executionPolicy = "full";
    return journal.upsertSession(owner);
}
// Separate read-only connection: checks committed SQL, not a cached projection.
QJsonObject stored(const QString& path, const QString& id) {
    const auto name = QUuid::createUuid().toString();
    QJsonObject result;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        db.setConnectOptions("QSQLITE_OPEN_READONLY");
        if (db.open()) {
            QSqlQuery query(db);
            query.prepare("SELECT holds_slot FROM managed_jobs WHERE task_id=?");
            query.addBindValue(id);
            if (query.exec() && query.next())
                result.insert("holds_slot", query.value(0).toInt());
            query.prepare("SELECT revision,source,user_client_id FROM managed_job_cleanup_audit "
                          "WHERE task_id=? ORDER BY revision");
            query.addBindValue(id);
            QJsonArray audit;
            if (query.exec()) {
                while (query.next())
                    audit.append(QJsonObject{{"revision", query.value(0).toLongLong()},
                                             {"source", query.value(1).toString()},
                                             {"user_client_id", query.value(2).toString()}});
                result.insert("audit", audit);
            }
            query.prepare("SELECT reason FROM managed_job_owner_fences "
                          "WHERE owner_session_id='owner' AND owner_generation=1");
            const bool fenceRead = query.exec();
            result.insert("owner_fence_read_ok", fenceRead);
            if (fenceRead && query.next())
                result.insert("owner_fence", query.value(0).toString());
            if (query.exec("SELECT count(*) FROM session_tombstones "
                           "WHERE session_id='owner' AND generation=1") &&
                query.next())
                result.insert("owner_tombstones", query.value(0).toInt());
            query.prepare("SELECT count(*) FROM managed_job_outbox WHERE task_id=?");
            query.addBindValue(id);
            if (query.exec() && query.next())
                result.insert("outbox_count", query.value(0).toInt());
        }
    }
    QSqlDatabase::removeDatabase(name);
    return result;
}
bool noOwnerInvalidation(const QString& path, const QString& id) {
    const auto snapshot = stored(path, id);
    return snapshot.value("owner_fence_read_ok").toBool() && !snapshot.contains("owner_fence") &&
           snapshot.value("owner_tombstones").toInt(-1) == 0;
}
bool noTurnEvents(const QSignalSpy& events) {
    for (const auto& values : events) {
        const auto type = values.first().toJsonObject().value("event_type").toString();
        if (type.startsWith("turn.") || type == "user.message")
            return false;
    }
    return true;
}
// ADR 0127 §7a: entering needs_attention wakes the owner with one notice. True when that
// notice started exactly one turn and no other message started one.
bool onlyTheAttentionNoticeWoke(const QSignalSpy& events, const QString& id) {
    int notices = 0;
    for (const auto& values : events) {
        const auto event = values.first().toJsonObject();
        if (event.value("event_type") != "user.message")
            continue;
        if (!event.value("payload")
                 .toObject()
                 .value("background_task_event_id")
                 .toString()
                 .startsWith(id + ":attention:"))
            return false;
        ++notices;
    }
    return notices == 1;
}
bool preservesOutput(const QJsonObject& page, const QJsonObject& before) {
    if (!page.value("ok").toBool() || !before.value("ok").toBool())
        return false;
    for (const auto* key : {"output", "next_cursor", "total_bytes", "truncated"})
        if (page.value(key) != before.value(key))
            return false;
    return true;
}
bool publishedIncident(const QSignalSpy& events, const QString& id, qint64 previousRevision) {
    for (const auto& values : events) {
        const auto event = values.first().toJsonObject();
        if (event.value("event_type") != "tasks.changed" || event.value("session_id") != "owner" ||
            event.value("session_generation").toInteger() != 1)
            continue;
        for (const auto value : event.value("payload").toObject().value("tasks").toArray()) {
            const auto incident = value.toObject();
            if (incident.value("task_id") != id || incident.value("owner_session_id") != "owner" ||
                incident.value("owner_generation").toInteger() != 1 ||
                incident.value("state") != "needs_attention" ||
                !incident.value("executor_stopped").toBool())
                continue;
#ifdef Q_OS_MACOS
            if (incident.value("revision").toInteger() > previousRevision)
#else
            // No-identity portable reconciliation publishes the unchanged incident.
            if (incident.value("revision").toInteger() == previousRevision)
#endif
                return true;
        }
    }
    return false;
}
#ifdef Q_OS_MACOS
executor::NativeProcessIdentity identity(const QJsonObject& value) {
    return {value.value("pid").toInteger(), 0, 0, value.value("birth").toString()};
}
bool stopped(const QJsonObject& value) {
    const auto now = executor::inspect(value.value("pid").toInteger());
    return !now || now->birth != value.value("birth").toString() || now->zombie;
}
#endif
} // namespace

class ManagedTaskAttentionRecoveryTest final : public QObject {
    Q_OBJECT
  private slots:
#ifdef Q_OS_LINUX
    void acceptedIntentWithoutCallerIdIsRecoverable() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.path() + "/journal.sqlite";
        QString expected;
        {
            LocalJournal journal(path);
            QVERIFY(journal.isOpen());
            QVERIFY(seed(journal, directory.path()));
            BackgroundTaskIntent intent{{"owner", 1, "fixture"},
                                        "attention",
                                        "process",
                                        "Lost callback",
                                        "fixture",
                                        60000,
                                        {}};
            const auto accepted = journal.backgroundTasks().create(intent);
            QVERIFY(accepted.value("ok").toBool());
            expected = accepted.value("task").toObject().value("task_id").toString();
            QVERIFY(!expected.isEmpty());
        }
        // No callback ID, open Host or executor: recover only the committed intent.
        QString callerId;
        QCOMPARE(attention_fixture::resolveCleanupIntent(path, "owner", 1, "attention", callerId),
                 attention_fixture::IntentLookup::Found);
        QCOMPARE(callerId, expected);
        QString absentId;
        QCOMPARE(attention_fixture::resolveCleanupIntent(path, "owner", 2, "attention", absentId),
                 attention_fixture::IntentLookup::Absent);
        QVERIFY(absentId.isEmpty());
        QCOMPARE(attention_fixture::resolveCleanupIntent(directory.path() + "/missing.sqlite",
                                                         "owner", 1, "attention", absentId),
                 attention_fixture::IntentLookup::Error);
        QVERIFY(absentId.isEmpty());
        QVERIFY(!QFileInfo::exists(directory.path() + "/missing.sqlite"));
    }
#endif
    void attentionSurvivesOwnerLifecycle_data() {
        QTest::addColumn<bool>("replaceGeneration");
        QTest::newRow("owner-close") << false;
        QTest::newRow("generation-change") << true;
    }

    void attentionSurvivesOwnerLifecycle() {
        QFETCH(bool, replaceGeneration);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto cwd = QFileInfo(directory.path()).canonicalFilePath();
        const auto databasePath = cwd + "/journal.sqlite";
        const auto providerLog = cwd + "/provider.log";
        const auto providerPath = cwd + "/fake-codex";
        QVERIFY(writeFakeCodex(providerPath, providerLog));
        QString id;
        QString originalAgentToken;
        QString phase = "initial";
        QJsonObject original;
        QJsonObject latest;
        QJsonObject root;
        QJsonObject supervisor;
        QByteArray output;
        QJsonObject outputPage;
        qint64 beforeOwnerChangeRevision = 0;
        bool cleanupClosed = true;
        int cleanupSignalFailures = 0;
        bool cleanupStopFailed = false;
        bool fixtureRetained = false;
        QElapsedTimer elapsed;
        elapsed.start();
        const auto diagnostic = qScopeGuard([&] {
            // Whitelisted, bounded fields only: never journal/receipt/protocol bodies,
            // execution spec, lease, provider text or arbitrary command arguments.
            qInfo().noquote()
                << QJsonDocument(
                       QJsonObject{
                           {"row", replaceGeneration ? "generation-change" : "owner-close"},
#ifdef Q_OS_MACOS
                           {"mode", "real-native-fault"},
#else
                           {"mode", "portable-injected-attention"},
#endif
                           {"phase", phase},
                           {"elapsed_ms", elapsed.elapsed()},
                           {"execution_event_timestamps_available", false},
                           {"task_id", id},
                           {"state", latest.value("state")},
                           {"revision", latest.value("revision")},
                           {"owner_generation", latest.value("owner_generation")},
                           {"executor_stopped", latest.value("executor_stopped")},
                           {"cleanup_status", latest.value("cleanup_status")},
                           {"exit_code", latest.value("exit_code")},
                           {"identity_present",
                            !latest.value("execution_identity").toObject().isEmpty()},
                           {"launch_marker_bytes", bytes(cwd + "/launch-marker").size()},
                           {"output_bytes", output.size()},
                           {"cleanup_closed", cleanupClosed},
                           {"cleanup_signal_failures", cleanupSignalFailures},
                           {"cleanup_stop_failed", cleanupStopFailed},
                           {"fixture_retained", fixtureRetained},
                           {"retained_fixture", fixtureRetained ? QJsonValue(cwd) : QJsonValue()}})
                       .toJson(QJsonDocument::Compact);
        });
#ifdef Q_OS_MACOS
        const auto cleanupNative = [&](LocalJournal& journal, SessionHost& host) {
            const auto result = attention_fixture::cleanupNativeAttention(
                journal, id, cwd, [&](const QString& taskId) {
                    return command(host, "session.task.stop", {{ "task_id", taskId }});
                });
            cleanupClosed = cleanupClosed && result.closed;
            cleanupSignalFailures += result.signalFailures;
            cleanupStopFailed = cleanupStopFailed || result.stopFailed;
            if (!result.ok()) {
                // Keep the DB/receipt until intervention if closure is uncertain,
                // or retain evidence of any failed stop/signal instead of ignoring it.
                directory.setAutoRemove(false);
                fixtureRetained = true;
                QTest::qFail("Native fixture cleanup failed; private directory retained.", __FILE__,
                             __LINE__);
            }
        };
#endif
        {
            LocalJournal journal(databasePath);
            QVERIFY(journal.isOpen());
            QVERIFY(seed(journal, cwd));
#ifndef Q_OS_MACOS
            // Linux verifies portable journal/Host/CAS/audit plumbing only. No
            // launch or native identity claim is made by this injected executor result.
            auto ledger = journal.backgroundTasks();
            BackgroundTaskIntent intent{{"owner", 1, "fixture"},
                                        "attention",
                                        "process",
                                        "Injected attention",
                                        "fixture",
                                        60000,
                                        {}};
            id = ledger.create(intent).value("task").toObject().value("task_id").toString();
            QVERIFY(!id.isEmpty());
            BackgroundTaskChange approved;
            approved.state = "queued";
            approved.permissionApproved = true;
            QVERIFY(ledger.change(id, 1, approved).value("ok").toBool());
            const auto claimed = ledger.claimNext().value("task").toObject();
            QCOMPARE(claimed.value("task_id"), id);
            BackgroundTaskChange attention;
            attention.state = "needs_attention";
            attention.executorStopped = false;
            attention.exitCode =
                17; // Explicit synthetic root result, not an observed process exit.
            QVERIFY(ledger.change(id, claimed.value("revision").toInteger(), attention)
                        .value("ok")
                        .toBool());
            QCOMPARE(stored(databasePath, id).value("holds_slot").toInt(-1), 1);
            attention.executorStopped = true;
            const auto revision = ledger.runtimeTask(id).value("revision").toInteger();
            QVERIFY(ledger.change(id, revision, attention).value("ok").toBool());
            const auto spoolPath = cwd + "/background-jobs/" + id;
            QVERIFY(QDir().mkpath(spoolPath));
            const auto spoolName = QUuid::createUuid().toString();
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE", spoolName);
                db.setDatabaseName(spoolPath + "/output.sqlite");
                QVERIFY(db.open());
                BackgroundTaskOutput spool(db);
                QString error;
                QVERIFY(spool.initialize(&error));
                QVERIFY(spool.append(id, "attention-output\n", &error));
            }
            QSqlDatabase::removeDatabase(spoolName);
#endif
            CodexAdapter adapter(providerPath);
            SessionHost host(&journal, &adapter);
            QSignalSpy events(&host, &SessionHost::eventReady);
            const auto stopAdapter = qScopeGuard([&] { adapter.stop(); });
#ifdef Q_OS_MACOS
            // Runs before adapter/Host/journal destruction and before directory removal.
            const auto closeNative = qScopeGuard([&] { cleanupNative(journal, host); });
#endif
#ifdef Q_OS_MACOS
            phase = "native-launch";
            const auto accepted =
                command(host, "session.task.start",
                        {{"client_request_id", "attention"},
                         {"kind", "process"},
                         {"title", "Attention recovery fixture"},
                         {"timeout_ms", 60000},
                         { "execution_spec",
                           QJsonObject {
                               {"cwd", cwd},
                               {"env_delta", QJsonObject {}},
                               { "argv",
                                 QJsonArray{"/bin/sh", "-c",
                                            "printf X >> launch-marker; printf "
                                            "'attention-output\\n'; exec /bin/sleep 60"} }
                           } }});
            id = accepted.value("task").toObject().value("task_id").toString();
            QVERIFY2(accepted.value("ok").toBool(),
                     qPrintable(accepted.value("error_code").toString()));
            QVERIFY(!id.isEmpty());
            const auto observeLaunch = [&] {
                const auto record = task(host, id, &latest);
                const auto execution = record.value("execution_identity").toObject();
                const auto recordedRoot = execution.value("root_identity").toObject();
                const auto recordedSupervisor = execution.value("supervisor_identity").toObject();
                if (!recordedRoot.isEmpty())
                    root = recordedRoot;
                if (!recordedSupervisor.isEmpty())
                    supervisor = recordedSupervisor;
                return record;
            };
            QTRY_COMPARE_WITH_TIMEOUT(observeLaunch().value("state"), "running", 6000);
            QVERIFY(!root.value("birth").toString().isEmpty());
            QVERIFY(!supervisor.value("birth").toString().isEmpty());
            QTRY_COMPARE_WITH_TIMEOUT(bytes(cwd + "/launch-marker"), QByteArray("X"), 3000);
            QCOMPARE(stored(databasePath, id).value("holds_slot").toInt(-1), 1);
            // Marker precedes stdout in the shell: require the API spool sample
            // before faulting, so all later assertions test preservation.
            QTRY_COMPARE_WITH_TIMEOUT(command(host, "session.task.output",
                                              {{ "task_id",
                                                 id }})
                                          .value("output"),
                                      "attention-output\n", 3000);
            outputPage = command(host, "session.task.output", {{ "task_id", id }});
            QCOMPARE(outputPage.value("output"), "attention-output\n");
            QCOMPARE(outputPage.value("total_bytes").toInteger(), 17);
            QVERIFY(!outputPage.value("next_cursor").toString().isEmpty());
            output = outputPage.value("output").toString().toUtf8();
            phase = "supervisor-fault";
            QString error;
            QVERIFY2(executor::signalIdentity(identity(supervisor), SIGKILL, &error),
                     qPrintable(error));
            QTRY_VERIFY_WITH_TIMEOUT(task(host, id, &latest).value("state") == "needs_attention" &&
                                         task(host, id, &latest).value("executor_stopped").toBool(),
                                     ketplus::daemon::executor::NativeJobLeaseMs + 5000);
            QVERIFY(stopped(root));
            QVERIFY(stopped(supervisor));
#endif
            phase = "attention-before-reopen";
            original = latest = task(host, id, &latest);
            originalAgentToken = host.agentToken("owner");
            QVERIFY(!originalAgentToken.isEmpty());
            QJsonObject agentDenied;
            host.handleAgentCommandDeferred(
                originalAgentToken, "session.task.resolve_cleanup",
                {{"task_id", id}, {"revision", original.value("revision")}},
                [&](const QJsonObject& value) { agentDenied = value; });
            QCOMPARE(agentDenied.value("error_code"), "forbidden");
            QCOMPARE(original.value("state"), "needs_attention");
            QCOMPARE(original.value("cleanup_status"), "unverified");
            QVERIFY(original.value("exit_code").isDouble());
            QVERIFY(original.value("executor_stopped").toBool());
            QVERIFY(original.value("completion").isNull());
            QCOMPARE(stored(databasePath, id).value("holds_slot").toInt(-1), 0);
            QTRY_COMPARE(command(host, "session.task.output", {{"task_id", id}}).value("output"),
                         "attention-output\n");
            const auto afterFaultOutput = command(host, "session.task.output", {{"task_id", id}});
            if (outputPage.isEmpty()) { // Linux's explicit injected-result subset.
                outputPage = afterFaultOutput;
                output = outputPage.value("output").toString().toUtf8();
            }
            QVERIFY(preservesOutput(afterFaultOutput, outputPage));
            // One notice wakes the owner; a restart below must not deliver it again.
            QTRY_VERIFY_WITH_TIMEOUT(onlyTheAttentionNoticeWoke(events, id), 6000);
        } // Both Host/runtime and the file-backed journal connection really close.

        {
            phase = "first-reopen";
            LocalJournal journal(databasePath);
            QVERIFY(journal.isOpen());
            CodexAdapter adapter(providerPath);
            SessionHost host(&journal, &adapter);
            QSignalSpy events(&host, &SessionHost::eventReady);
            const auto stopAdapter = qScopeGuard([&] { adapter.stop(); });
#ifdef Q_OS_MACOS
            // Runs before adapter/Host/journal destruction and before directory removal.
            const auto closeNative = qScopeGuard([&] { cleanupNative(journal, host); });
#endif
            // Match this incident and generation; native publication must carry
            // a revision from reconciliation, not a persisted executor-stopped flag.
            QTRY_VERIFY_WITH_TIMEOUT(
                publishedIncident(events, id, original.value("revision").toInteger()),
                ketplus::daemon::executor::NativeJobLeaseMs + 5000);
            QTRY_VERIFY_WITH_TIMEOUT(task(host, id, &latest).value("executor_stopped").toBool(),
                                     ketplus::daemon::executor::NativeJobLeaseMs + 5000);
            latest = task(host, id, &latest);
            QCOMPARE(latest.value("state"), "needs_attention");
            QCOMPARE(latest.value("task_id"), id);
            QVERIFY(latest.value("execution_identity") == original.value("execution_identity"));
            QCOMPARE(latest.value("exit_code"), original.value("exit_code"));
            QCOMPARE(latest.value("cleanup_status"), "unverified");
            QCOMPARE(latest.value("owner_session_id"), "owner");
            QCOMPARE(latest.value("owner_generation").toInteger(), 1);
            QCOMPARE(stored(databasePath, id).value("audit").toArray().size(), 0);
            QVERIFY(noTurnEvents(events));
            QVERIFY(preservesOutput(command(host, "session.task.output", {{"task_id", id}}),
                                    outputPage));
            beforeOwnerChangeRevision = latest.value("revision").toInteger();
            if (!replaceGeneration) {
                phase = "host-owner-close";
                const auto closed =
                    command(host, "session.delete", {{"delete_operation_id", "close"}});
                QVERIFY2(closed.value("ok").toBool(),
                         qPrintable(closed.value("error_code").toString()));
                QVERIFY(!journal.sessionTombstone("owner", 1).sessionId.isEmpty());
                latest = task(host, id, &latest);
                QCOMPARE(latest.value("state"), "needs_attention");
            } else {
                // A close fence/tombstone must not mask generation mismatch.
                QVERIFY(journal.sessionTombstone("owner", 1).sessionId.isEmpty());
                QVERIFY(noOwnerInvalidation(databasePath, id));
            }
            QVERIFY(noTurnEvents(events));
        }
        if (replaceGeneration) {
            phase = "replacement-generation";
            LocalJournal journal(databasePath);
            QVERIFY(journal.sessionTombstone("owner", 1).sessionId.isEmpty());
            QVERIFY(noOwnerInvalidation(databasePath, id));
            QVERIFY(seed(journal, cwd, 2));
            QCOMPARE(journal.sessionHead("owner").generation, 2);
            QVERIFY(journal.sessionTombstone("owner", 1).sessionId.isEmpty());
            QVERIFY(noOwnerInvalidation(databasePath, id));
        }
        {
            phase = "owner-invalid-reopen";
            LocalJournal journal(databasePath);
            QVERIFY(journal.isOpen());
            CodexAdapter adapter(providerPath);
            SessionHost host(&journal, &adapter);
            QSignalSpy events(&host, &SessionHost::eventReady);
            const auto stopAdapter = qScopeGuard([&] { adapter.stop(); });
#ifdef Q_OS_MACOS
            // Runs before adapter/Host/journal destruction and before directory removal.
            const auto closeNative = qScopeGuard([&] { cleanupNative(journal, host); });
#endif
            // Polling the invalid owner durably fences it; this is a real Host
            // event-loop barrier before checking the retained incident.
            QTRY_COMPARE_WITH_TIMEOUT(stored(databasePath, id).value("owner_fence"),
                                      "owner_invalid", 6000);
#ifdef Q_OS_MACOS
            QTRY_VERIFY_WITH_TIMEOUT(task(host, id, &latest).value("revision").toInteger() >
                                         beforeOwnerChangeRevision,
                                     ketplus::daemon::executor::NativeJobLeaseMs + 5000);
#endif
            QTRY_VERIFY_WITH_TIMEOUT(task(host, id, &latest).value("executor_stopped").toBool(),
                                     6000);
            latest = task(host, id, &latest);
            QCOMPARE(latest.value("state"), "needs_attention");
            QCOMPARE(latest.value("task_id"), id);
            QVERIFY(latest.value("revision").toInteger() >= beforeOwnerChangeRevision);
            QCOMPARE(latest.value("cleanup_status"), "unverified");
            QCOMPARE(latest.value("owner_generation").toInteger(), 1);
            QVERIFY(latest.value("execution_identity") == original.value("execution_identity"));
            QCOMPARE(latest.value("exit_code"), original.value("exit_code"));
            const auto restoredOutput = command(host, "session.task.output", {{"task_id", id}});
            QCOMPARE(restoredOutput.value("output").toString().toUtf8(), output);
            QCOMPARE(restoredOutput.value("next_cursor"), outputPage.value("next_cursor"));
            QCOMPARE(restoredOutput.value("total_bytes"), outputPage.value("total_bytes"));
            QCOMPARE(restoredOutput.value("truncated"), outputPage.value("truncated"));
            const auto kept = journal.backgroundTasks().prune(QDateTime::currentMSecsSinceEpoch() +
                                                              8LL * 24 * 60 * 60 * 1000);
            QVERIFY(kept.value("ok").toBool());
            QCOMPARE(kept.value("pruned").toInt(), 0);
            QCOMPARE(journal.backgroundTasks().nonterminal().value("tasks").toArray().size(), 1);
            QCOMPARE(stored(databasePath, id).value("holds_slot").toInt(-1), 0);
            QCOMPARE(stored(databasePath, id).value("outbox_count").toInt(-1), 0);
            QCOMPARE(stored(databasePath, id).value("audit").toArray().size(), 0);
            const auto revision = latest.value("revision").toInteger();
            QJsonObject denied;
            host.handleAgentCommandDeferred(originalAgentToken, "session.task.resolve_cleanup",
                                            {{"task_id", id}, {"revision", revision}},
                                            [&](const QJsonObject& value) { denied = value; });
            QVERIFY(!denied.value("ok").toBool());
            QCOMPARE(command(host, "session.task.resolve_cleanup",
                             {{"task_id", id}, {"revision", revision - 1}})
                         .value("error_code"),
                     "revision_conflict");
            phase = "user-acknowledgement";
            const auto resolved = command(host, "session.task.resolve_cleanup",
                                          {{"task_id", id}, {"revision", revision}});
            QVERIFY2(resolved.value("ok").toBool(),
                     qPrintable(resolved.value("error_code").toString()));
            latest = task(host, id, &latest);
            QCOMPARE(latest.value("state"), "interrupted");
            QCOMPARE(latest.value("cleanup_status"), "unverified");
            QCOMPARE(latest.value("completion").toObject().value("status"), "suppressed");
            QVERIFY(!command(host, "session.task.resolve_cleanup",
                             {{"task_id", id}, {"revision", revision}})
                         .value("ok")
                         .toBool());
            const auto audit = stored(databasePath, id).value("audit").toArray();
            QCOMPARE(audit.size(), 1);
            QCOMPARE(audit.first().toObject().value("revision").toInteger(), revision + 1);
            QCOMPARE(audit.first().toObject().value("source"), "user");
            QCOMPARE(audit.first().toObject().value("user_client_id"), "attention-test-user");
            QVERIFY(noTurnEvents(events));
        }
        {
            phase = "final-reopen";
            LocalJournal journal(databasePath);
            QVERIFY(journal.isOpen());
            CodexAdapter adapter(providerPath);
            SessionHost host(&journal, &adapter);
            QSignalSpy events(&host, &SessionHost::eventReady);
            const auto stopAdapter = qScopeGuard([&] { adapter.stop(); });
#ifdef Q_OS_MACOS
            // Runs before adapter/Host/journal destruction and before directory removal.
            const auto closeNative = qScopeGuard([&] { cleanupNative(journal, host); });
#endif
            bool initializationDispatched = false;
            QTimer::singleShot(0, &host, [&] { initializationDispatched = true; });
            QTRY_VERIFY_WITH_TIMEOUT(initializationDispatched, 6000);
            // Exercise more than two 500 ms wake-dispatch polls after reopening
            // the suppressed completion, not just synchronous registry reads.
            QTest::qWait(1100);
            latest = task(host, id, &latest);
            QCOMPARE(latest.value("state"), "interrupted");
            QCOMPARE(latest.value("cleanup_status"), "unverified");
            QVERIFY(latest.value("execution_identity") == original.value("execution_identity"));
            QCOMPARE(latest.value("exit_code"), original.value("exit_code"));
            QCOMPARE(latest.value("owner_generation").toInteger(), 1);
            QCOMPARE(latest.value("completion").toObject().value("status"), "suppressed");
            QCOMPARE(stored(databasePath, id).value("audit").toArray().size(), 1);
            const auto restoredOutput = command(host, "session.task.output", {{"task_id", id}});
            QCOMPARE(restoredOutput.value("output").toString().toUtf8(), output);
            QCOMPARE(restoredOutput.value("next_cursor"), outputPage.value("next_cursor"));
            QCOMPARE(restoredOutput.value("total_bytes"), outputPage.value("total_bytes"));
            QCOMPARE(restoredOutput.value("truncated"), outputPage.value("truncated"));
            QVERIFY(
                journal.backgroundTasks().deliveryPage(false).value("events").toArray().isEmpty());
            QVERIFY(
                journal.backgroundTasks().deliveryPage(true).value("events").toArray().isEmpty());
            QVERIFY(noTurnEvents(events));
        }
#ifdef Q_OS_MACOS
        QCOMPARE(bytes(cwd + "/launch-marker"), QByteArray("X"));
        QVERIFY(stopped(root));
        QVERIFY(stopped(supervisor));
#endif
        QVERIFY(!bytes(providerLog).contains("turn:"));
        phase = "complete";
    }
};

QTEST_GUILESS_MAIN(ManagedTaskAttentionRecoveryTest)
#include "ManagedTaskAttentionRecoveryTest.moc"
