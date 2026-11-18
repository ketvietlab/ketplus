#pragma once

#include "FakeCodex.h"
#include "RemoteJobsRunnerEnd.h"
#include "providers/CodexAdapter.h"
#include "runner/entry/RemoteJobEntry.h"
#include "runtime/ManagedTaskRuntime.h"
#include "runtime/SessionHost.h"
#include "storage/BackgroundTaskLedger.h"
#include "storage/LocalJournal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include <functional>
#include <memory>
#include <optional>

// The session host, its bridge and runtime on a real journal, joined to a real remote job entry
// and real remote jobs on that journal's registry; the Relay and the runner are played by the
// test (RemoteJobsRunnerEnd), and remote job time moves only when the test moves it.
namespace managed_remote_test {

using namespace ketplus::daemon;
using namespace remote_jobs_test;
using runner::entry::RemoteJobEntry;

inline QJsonObject wait(const std::shared_ptr<QJsonObject>& result) {
    QElapsedTimer timer;
    timer.start();
    while (result->isEmpty() && timer.elapsed() < 6000)
        QTest::qWait(10);
    return *result;
}

struct Joined {
    QTemporaryDir directory;
    std::unique_ptr<LocalJournal> journal;
    std::optional<BackgroundTaskLedger> registry;
    std::unique_ptr<CodexAdapter> adapter;
    std::unique_ptr<EndToEnd> e;
    std::unique_ptr<RemoteJobEntry> entry;
    std::unique_ptr<SessionHost> host;
    std::unique_ptr<QSignalSpy> events; // the host's events since the last start or restart
    QString cwd;                        // a directory of the worktree the snapshot is taken of
    QString codex;

    explicit Joined(const QString& policy = QStringLiteral("ask")) {
        codex = directory.filePath(fixtureName("fake-codex"));
        if (!writeFakeCodex(codex, directory.filePath("provider.log")))
            qFatal("fake codex");
        journal = std::make_unique<LocalJournal>(directory.filePath("journal.sqlite"));
        registry.emplace(journal->backgroundTasks());
        e = std::make_unique<EndToEnd>([this](RemoteHarness& h) {
            h.registry = RemoteHarness::Registry{registry->database(), &*registry};
        });
        cwd = QDir(e->repo).filePath(QStringLiteral("src"));
        if (!seed(QStringLiteral("owner"), policy) || !seed(QStringLiteral("other"), policy))
            qFatal("seed");
        adapter = std::make_unique<CodexAdapter>(codex);
        host = std::make_unique<SessionHost>(journal.get(), adapter.get());
        attach();
    }
    // The provider stops while its host lives: a turn it fails answers into the host.
    ~Joined() {
        if (host)
            host->setRemoteJobs(nullptr);
        adapter->stop();
        host.reset();
        entry.reset();
        e.reset();
    }
    bool seed(const QString& id, const QString& policy) {
        StoredSession session;
        session.id = id;
        session.worktreePath = e->repo;
        session.adapter = QStringLiteral("codex");
        session.state = QStringLiteral("ready");
        session.executionPolicy = policy;
        return journal->upsertSession(session);
    }
    void attach() {
        entry = std::make_unique<RemoteJobEntry>(*e->h.jobs);
        e->h.clock.target = &e->h.jobs->coordinator();
        events = std::make_unique<QSignalSpy>(host.get(), &SessionHost::eventReady);
        host->setRemoteJobs(entry.get());
    }
    // A daemon restart: the host and its runtime first, then the remote jobs, which end or keep
    // their records before the host gets their entry, in the order the daemon starts them.
    void restart() {
        host->setRemoteJobs(nullptr);
        adapter->stop(); // the provider of the stopped daemon goes with it
        host.reset();
        entry.reset();
        adapter = std::make_unique<CodexAdapter>(codex);
        host = std::make_unique<SessionHost>(journal.get(), adapter.get());
        e->h.build();
        e->h.clock.due.clear(); // the timers of the stopped daemon end with it
        e->h.jobs->restore();
        attach();
    }

    QJsonObject run(const QString& type, const QJsonObject& payload = {},
                    const QString& session = QStringLiteral("owner"), const qint64 generation = 1,
                    const QString& client = QStringLiteral("local-test")) {
        auto result = std::make_shared<QJsonObject>();
        host->handleCommandDeferred({{"message_id", QUuid::createUuid().toString()},
                                     {"message_type", type},
                                     {"session_id", session},
                                     {"session_generation", generation},
                                     {"payload", payload}},
                                    client,
                                    [result](const QJsonObject& reply) { *result = reply; });
        return wait(result);
    }
    // A command of the agent of the session that holds `token`, without a mutation origin.
    QJsonObject agent(const QString& token, const QString& type, const QJsonObject& arguments) {
        auto result = std::make_shared<QJsonObject>();
        host->handleAgentCommandDeferred(token, type, arguments,
                                         [result](const QJsonObject& reply) { *result = reply; });
        return wait(result);
    }
    // Pumps the event loop and the remote jobs' deferred work until `done` holds.
    bool until(const std::function<bool()>& done, const int ms = 6000) {
        QElapsedTimer timer;
        timer.start();
        while (!done()) {
            if (timer.elapsed() > ms)
                return false;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            e->h.run();
        }
        return true;
    }
    QString state(const QString& id) { return task(id).value("state").toString(); }
    QJsonObject task(const QString& id) {
        return run("session.task.get", {{"task_id", id}}).value("task").toObject();
    }
    QString output(const QString& id) {
        return run("session.task.output", {{"task_id", id}}).value("output").toString();
    }

    [[nodiscard]] QJsonObject process(const QString& request = QStringLiteral("request"),
                                      const qint64 timeoutMs = 60'000) const {
        return {{"client_request_id", request},
                {"kind", "process"},
                {"title", "Build"},
                {"timeout_ms", timeoutMs},
                {"execution_spec", QJsonObject{{"argv", QJsonArray{"/usr/bin/printf", "local"}},
                                               {"cwd", cwd},
                                               {"env_delta", QJsonObject{}}}}};
    }
    [[nodiscard]] QJsonObject remote(const QString& request = QStringLiteral("remote"),
                                     const qint64 timeoutMs = 60'000) const {
        QJsonObject payload = process(request, timeoutMs);
        payload.insert("runs_on", QJsonArray{"os:linux"});
        return payload;
    }

    QJsonArray approvals() {
        return run("session.pending_approvals").value("pending_approvals").toArray();
    }
    bool resolve(const QString& decision) {
        const auto pending = approvals();
        if (pending.isEmpty())
            return false;
        return run("approval.resolve",
                   {{"approval_id", pending.first().toObject().value("approval_id")},
                    {"resolution", QJsonObject{{"decision", decision}}}})
            .value("ok")
            .toBool();
    }

    // A remote task started and approved, its job.submit held by the Relay; empty on failure.
    QString approved(const QString& request = QStringLiteral("remote"),
                     const qint64 timeoutMs = 60'000) {
        const auto started = run("session.task.start", remote(request, timeoutMs));
        const QString id = started.value("task").toObject().value("task_id").toString();
        if (id.isEmpty()) {
            qWarning() << "start refused" << QJsonDocument(started).toJson(QJsonDocument::Compact);
            return {};
        }
        if (!until([&] { return approvals().size() == 1; }) || !resolve(QStringLiteral("accept")) ||
            !until([&] { return e->h.relay.pending(QStringLiteral("submit")) == 1; })) {
            qWarning() << "not submitted" << QJsonDocument(task(id)).toJson();
            return {};
        }
        return id;
    }
    // As approved, the Relay then answers the submit: queued as linkedJob.
    QString queued(const QString& request = QStringLiteral("remote"),
                   const qint64 timeoutMs = 60'000) {
        const QString id = approved(request, timeoutMs);
        if (id.isEmpty())
            return {};
        remote_job_test::RelayJob job = remote_job_test::relayJob(QStringLiteral("queued"));
        job.jobId = linkedJob;
        return e->h.relay.answer(QStringLiteral("submit"), job) ? id : QString();
    }
    // As queued, then linkedRunner takes the job, accepts its spec and its snapshot, and the
    // command starts: running, with `end` the open peer connection.
    QString running(std::unique_ptr<RunnerEnd>& end, const qint64 timeoutMs = 60'000) {
        const QString id = queued(QStringLiteral("remote"), timeoutMs);
        if (id.isEmpty())
            return {};
        e->assign();
        end = std::make_unique<RunnerEnd>(e->connect(1));
        if (!e->play(*end, [&] { return e->ready; }, false))
            return {};
        end->send(protocol::JobStarted{linkedJob, utc(e->parts.clock.time)});
        e->started = true;
        if (!until([&] { return state(id) == "running"; }))
            return {};
        return id;
    }

    int count(const QString& statement) {
        QSqlQuery query(registry->database());
        if (!query.exec(statement) || !query.next())
            return -1;
        return query.value(0).toInt();
    }
    int completions(const QString& id) {
        return count("SELECT count(*) FROM managed_job_outbox WHERE task_id='" + id + "'");
    }
    int rows(const QString& table) { return count("SELECT count(*) FROM " + table); }
    // The host's tasks.changed events naming `id` since the last attach.
    int changes(const QString& id) {
        int found = 0;
        for (const QList<QVariant>& event : *events) {
            const QJsonObject body = event.value(0).toJsonObject();
            if (body.value("event_type") != "tasks.changed")
                continue;
            for (const auto value : body.value("payload").toObject().value("tasks").toArray())
                found += value.toObject().value("task_id") == id ? 1 : 0;
        }
        return found;
    }
    ManagedTaskRuntime* runtime() const {
        for (auto* child : host->findChildren<QObject*>())
            if (auto* found = dynamic_cast<ManagedTaskRuntime*>(child))
                return found;
        return nullptr;
    }
};

inline protocol::JobResult resultOf(const QString& state,
                                    const std::optional<qint64> exitCode = {}) {
    protocol::JobResult result;
    result.jobId = linkedJob;
    result.state = state;
    result.exitCode = exitCode;
    result.cleanupStatus = QStringLiteral("verified");
    return result;
}

} // namespace managed_remote_test
