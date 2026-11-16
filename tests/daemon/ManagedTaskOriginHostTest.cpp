#include "providers/CodexAdapter.h"
#include "providers/CopilotAdapter.h"
#include "providers/CursorAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtTest>
#include <utility>

namespace ketplus::daemon {
namespace {
class CatalogFixture final : public ProviderAdapter {
  public:
    QString name() const override { return "cursor"; }
    QString executable() const override { return "fixture"; }
    QString missingMessage() const override { return "Fixture missing"; }
    bool isAvailable() const override { return true; }
    bool isReady() const override { return true; }
    bool supports(const QString&) const override { return true; }
    bool supportsManagedMutations() const override { return true; } // Trusted synthetic bootstrap.
    void runtimeStatus(std::function<void(const compat::Runtime&)> done) override { done({}); }
    void configurationCatalog(Callback done) override { pending = std::move(done); }
    void releaseCatalog() {
        const auto done = std::exchange(pending, {});
        done({{"catalog",
               QJsonArray{QJsonObject{{"id", "fixture-model"}, {"efforts", QJsonArray{}}}}}},
             {});
    }
    void call(const QString& method, const QJsonObject& params, Callback done) override {
        const auto session = params.value("sessionId").toString();
        if (method == "turn/start") {
            done({{"thread", QJsonObject{{"id", "fixture-thread:" + session}}},
                  {"turn", QJsonObject{{"id", "fixture-turn:" + session}}}},
                 {});
        } else {
            done({}, {});
            if (method == "turn/interrupt")
                emit canonicalEvent(session, "turn.completed", {{"status", "interrupted"}});
        }
    }
    void respond(const QJsonValue&, const QJsonObject&) override {}
    void respondError(const QJsonValue&, const QString&, const QString&) override {}
    void stop() override {}
    bool releaseSessionTransport(const QString& session) override {
        released.append(session);
        return allowRelease;
    }
    bool allowRelease = true;
    QStringList released;
    Callback pending;
};

bool seed(LocalJournal& journal, const QString& cwd, const QString& provider = "cursor") {
    StoredSession owner;
    owner.id = "owner";
    owner.worktreePath = cwd;
    owner.adapter = provider;
    owner.model = "fixture-model";
    owner.state = "ready";
    owner.executionPolicy = "full";
    return journal.upsertSession(owner);
}
QJsonObject child(const QString& request = "request") {
    return {{"client_request_id", request},
            {"kind", "agent"},
            {"title", "Fixture child"},
            {"timeout_ms", 10000},
            {"execution_spec", QJsonObject{{"prompt", "fixture"}}}};
}
QJsonObject invoke(SessionHost& host, const QString& token, const QString& type,
                   const QJsonObject& arguments) {
    auto reply = std::make_shared<QJsonObject>();
    host.handleAgentCommandDeferred(token, type, arguments,
                                    [reply](const QJsonObject& value) { *reply = value; });
    QElapsedTimer timer;
    timer.start();
    while (reply->isEmpty() && timer.elapsed() < 2000)
        QTest::qWait(10);
    return *reply;
}
QJsonObject local(SessionHost& host, const QString& type, const QJsonObject& payload = {}) {
    auto reply = std::make_shared<QJsonObject>();
    host.handleCommandDeferred({{"message_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                {"message_type", type},
                                {"session_id", "owner"},
                                {"session_generation", 1},
                                {"payload", payload}},
                               "local-test", [reply](const QJsonObject& value) { *reply = value; });
    QElapsedTimer timer;
    timer.start();
    while (reply->isEmpty() && timer.elapsed() < 2000)
        QTest::qWait(10);
    return *reply;
}
} // namespace

class ManagedTaskOriginHostTest final : public QObject {
    Q_OBJECT
    // This fixture seeds the trusted adapter ACK directly. Adapter refresh and
    // real-provider preservation have separate tests; no model is requested here.
    static AgentMutationOrigin::Bootstrap origin(SessionHost& host, qint64 turn,
                                                 const QString& policy, bool review) {
        auto& owner = host.sessions_["owner"];
        owner.stored.currentTurnSequence = turn;
        owner.stored.turnId = "fixture-origin:" + QString::number(turn);
        owner.stored.state = "running";
        owner.stopRequested = false;
        owner.interruptPending = false;
        const auto bootstrap = owner.mutationOrigin.prepare("owner", 1, turn, policy, review);
        owner.mutationOrigin.activate(bootstrap.transportEpoch);
        return bootstrap;
    }

  private slots:
    void unverifiedConcreteAcpAdaptersNeverReceiveMutationCredentials_data() {
        QTest::addColumn<QString>("name");
        QTest::newRow("cursor") << "cursor";
        QTest::newRow("copilot") << "copilot";
    }
    void unverifiedConcreteAcpAdaptersNeverReceiveMutationCredentials() {
        QFETCH(QString, name);
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(seed(journal, directory.path(), name));
        CodexAdapter codex(directory.filePath("missing-codex"));
        CursorAdapter cursor(directory.filePath("missing-cursor"));
        CopilotAdapter copilot(directory.filePath("missing-copilot"));
        SessionHost host(&journal, &codex, QList<ProviderAdapter*>{&cursor, &copilot});
        host.setAgentEndpoint(directory.filePath("socket"), directory.filePath("mcp-helper"));
        auto& owner = host.sessions_["owner"];
        owner.stored.currentTurnSequence = 1;
        owner.stored.state = "running";
        host.prepareManagedTaskOrigin(owner,
                                      {{"execution_policy", "full"}, {"review_guard", false}});
        const auto bootstrap = owner.mutationOrigin.bootstrap();
        QVERIFY(!bootstrap.transportEpoch.isEmpty());
        QVERIFY(bootstrap.credential.isEmpty());
        QVERIFY(host.acknowledgeManagedTaskOrigin(
            owner, bootstrap.transportEpoch,
            {{"managedTransportEpoch", bootstrap.transportEpoch}}));
        QVERIFY(!owner.mutationOrigin.authorize("spoofed", "owner", 1));
        const auto launch = host.agentLaunch("owner", 1);
        QVERIFY(launch && launch->mutationCredential.isEmpty());
        const auto token = host.agentToken("owner");
        QCOMPARE(invoke(host, token, "session.task.start", child()).value("error_code"),
                 "mutation_refresh_unavailable");
        QCOMPARE(
            invoke(host, token, "session.task.stop", {{"task_id", "fixture"}}).value("error_code"),
            "mutation_refresh_unavailable");
        QVERIFY(invoke(host, token, "session.task.list", {}).value("ok").toBool());
    }

    void lateEpochCallbackCannotAcknowledgeReplacementOrMissingRefresh() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(seed(journal, directory.path(), "codex"));
        CodexAdapter codex(directory.filePath("missing-codex"));
        SessionHost host(&journal, &codex);
        host.setAgentEndpoint(directory.filePath("socket"), directory.filePath("mcp-helper"));
        auto& owner = host.sessions_["owner"];
        owner.stored.currentTurnSequence = 1;
        host.prepareManagedTaskOrigin(owner, {{"execution_policy", "full"}});
        const auto first = owner.mutationOrigin.bootstrap();
        QVERIFY(!first.credential.isEmpty());
        owner.stored.currentTurnSequence = 2;
        host.prepareManagedTaskOrigin(owner,
                                      {{"execution_policy", "full"}, {"review_guard", true}});
        const auto review = owner.mutationOrigin.bootstrap();
        // The same guard runs before every provider setup callback can touch Host state.
        QVERIFY(!host.isManagedTaskEpoch(owner, first.transportEpoch));
        QVERIFY(host.isManagedTaskEpoch(owner, review.transportEpoch));
        QVERIFY(!owner.mutationOrigin.authorize(first.credential, "owner", 1));
        owner.stored.currentTurnSequence = 3;
        host.prepareManagedTaskOrigin(owner, {{"execution_policy", "full"}});
        const auto pending = owner.mutationOrigin.bootstrap();
        QVERIFY(!host.isManagedTaskEpoch(owner, review.transportEpoch));
        bool staleDelivered = false;
        QTimer::singleShot(0, &host, [&] {
            if (host.isManagedTaskEpoch(owner, review.transportEpoch))
                host.acknowledgeManagedTaskOrigin(
                    owner, review.transportEpoch,
                    {{"managedTransportEpoch", review.transportEpoch}});
            staleDelivered = true;
        });
        QTRY_VERIFY(staleDelivered);
        QCOMPARE(owner.mutationOrigin.bootstrap().transportEpoch, pending.transportEpoch);
        QVERIFY(!owner.mutationOrigin.authorize(pending.credential, "owner", 1));
        QVERIFY(!host.acknowledgeManagedTaskOrigin(owner, pending.transportEpoch, {}));
        QCOMPARE(owner.stored.state, "failed");
        QVERIFY(!owner.mutationOrigin.authorize(pending.credential, "owner", 1));
    }

    void deletionWaitsForScopedTransportRelease() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(seed(journal, directory.path()));
        CodexAdapter codex(directory.filePath("missing-codex"));
        CatalogFixture provider;
        SessionHost host(&journal, &codex, QList<ProviderAdapter*>{&provider});
        provider.allowRelease = false;
        QCOMPARE(local(host, "session.delete").value("error_code"), "transport_close_failed");
        QVERIFY(host.sessions_.contains("owner"));
        provider.allowRelease = true;
        const auto deleted = local(host, "session.delete");
        QVERIFY2(deleted.value("ok").toBool(), qPrintable(QJsonDocument(deleted).toJson()));
        QVERIFY(!host.sessions_.contains("owner"));
        QCOMPARE(provider.released, QStringList({"owner", "owner"}));
    }

    void reviewAndDelayedOriginsCannotUseStoredFullPolicy_data() {
        QTest::addColumn<QString>("provider");
        for (const auto* provider : {"claude", "codex", "cursor", "copilot"})
            QTest::newRow(provider) << provider;
    }
    void reviewAndDelayedOriginsCannotUseStoredFullPolicy() {
        QFETCH(QString, provider);
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(seed(journal, directory.path(), provider));
        CodexAdapter codex(directory.filePath("missing-codex"));
        SessionHost host(&journal, &codex);
        const auto token = host.agentToken("owner");
        const auto old = origin(host, 1, "full", false);
        for (const auto* profile : {"read", "verify"}) {
            const auto review = origin(host, 2, "full", true);
            QVERIFY(review.credential.isEmpty());
            for (const auto* phase : {"running", "completed", "idle", "drain"}) {
                host.sessions_["owner"].stored.state = phase;
                auto request = child();
                request.insert("review_profile", "normal");
                request.insert("turn_id", "another-turn");
                request.insert(mutationOriginField, old.credential);
                QCOMPARE(invoke(host, token, "session.task.start", request).value("error_code"),
                         "stale_origin");
                request.insert("kind", "process");
                QCOMPARE(invoke(host, token, "session.task.start", request).value("error_code"),
                         "stale_origin");
                QCOMPARE(invoke(host, token, "session.task.stop", request).value("error_code"),
                         "stale_origin");
            }
            const auto normal = origin(host, 3, "full", false);
            QVERIFY(!normal.credential.isEmpty());
            QCOMPARE(invoke(host, token, "session.task.start",
                            {{mutationOriginField, review.credential}})
                         .value("error_code"),
                     "stale_origin");
            QCOMPARE(
                invoke(host, token, "session.task.start", {{mutationOriginField, old.credential}})
                    .value("error_code"),
                "stale_origin");
            QVERIFY(invoke(host, token, "session.task.list", {}).value("ok").toBool());
            QCOMPARE(invoke(host, token, "session.task.resolve_cleanup", {}).value("error_code"),
                     "forbidden");
            Q_UNUSED(profile);
        }
    }

    void queuedCatalogRequestCannotBecomeANewTurnRequest() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(seed(journal, directory.path()));
        CodexAdapter codex(directory.filePath("missing-codex"));
        CatalogFixture provider;
        SessionHost host(&journal, &codex, QList<ProviderAdapter*>{&provider});
        const auto token = host.agentToken("owner");
        const auto first = origin(host, 1, "ask", false);
        auto request = child();
        request.insert(mutationOriginField, first.credential);
        QJsonObject reply;
        host.handleAgentCommandDeferred(token, "session.task.start", request,
                                        [&](const QJsonObject& value) { reply = value; });
        QTRY_VERIFY(provider.pending || !reply.isEmpty());
        QVERIFY2(provider.pending, qPrintable(QJsonDocument(reply).toJson()));
        QVERIFY(reply.isEmpty());
        QVERIFY(
            host.appendEvent(host.sessions_["owner"], "turn.completed", {{"status", "completed"}}));
        origin(host, 2, "full", true);
        origin(host, 3, "full", false);
        provider.releaseCatalog();
        QTRY_VERIFY(!reply.isEmpty());
        QCOMPARE(reply.value("error_code"), "stale_origin");
        const auto listed = invoke(host, token, "session.task.list", {});
        QVERIFY(listed.value("ok").toBool());
        QVERIFY(listed.value("tasks").toArray().isEmpty());
    }

    void authorizedChildKeepsAcceptedPolicySnapshot() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(seed(journal, directory.path()));
        CodexAdapter codex(directory.filePath("missing-codex"));
        CatalogFixture provider;
        SessionHost host(&journal, &codex, QList<ProviderAdapter*>{&provider});
        const auto token = host.agentToken("owner");
        const auto first = origin(host, 1, "ask", false);
        auto request = child();
        request.insert(mutationOriginField, first.credential);
        QJsonObject reply;
        host.handleAgentCommandDeferred(token, "session.task.start", request,
                                        [&](const QJsonObject& value) { reply = value; });
        QTRY_VERIFY(provider.pending || !reply.isEmpty());
        QVERIFY2(provider.pending, qPrintable(QJsonDocument(reply).toJson()));
        host.sessions_["owner"].stored.executionPolicy = "full";
        provider.releaseCatalog();
        QTRY_VERIFY(!reply.isEmpty());
        QVERIFY2(reply.value("ok").toBool(), qPrintable(QJsonDocument(reply).toJson()));
        const auto task = reply.value("task").toObject();
        QCOMPARE(task.value("execution").toObject().value("origin_execution_policy"), "ask");
        const auto id = task.value("task_id").toString();
        const auto duplicate = invoke(host, token, "session.task.start", request);
        QVERIFY(duplicate.value("ok").toBool());
        QCOMPARE(duplicate.value("task").toObject().value("task_id"), id);
        origin(host, 2, "full", true);
        QJsonArray approvals;
        QTRY_VERIFY(
            !(approvals =
                  local(host, "session.pending_approvals").value("pending_approvals").toArray())
                 .isEmpty());
        // Current owner is full, but the accepted ask policy still requires an
        // explicit user approval. It cannot authorize itself after policy changes.
        QVERIFY(local(host, "approval.resolve",
                      {{"approval_id", approvals.first().toObject().value("approval_id")},
                       {"decision", "accept"}})
                    .value("ok")
                    .toBool());
        QString childId;
        QTRY_VERIFY(!(childId = invoke(host, token, "session.task.get", {{"task_id", id}})
                                    .value("task")
                                    .toObject()
                                    .value("execution_identity")
                                    .toObject()
                                    .value("child_session_id")
                                    .toString())
                         .isEmpty());
        QCOMPARE(host.sessions_.value(childId).stored.executionPolicy, "ask");
        QCOMPARE(invoke(host, token, "session.task.stop",
                        {{"task_id", id}, {mutationOriginField, first.credential}})
                     .value("error_code"),
                 "stale_origin");
        const auto normal = origin(host, 3, "full", false);
        QVERIFY(invoke(host, token, "session.task.stop",
                       {{"task_id", id}, {mutationOriginField, normal.credential}})
                    .value("ok")
                    .toBool());
    }
};
} // namespace ketplus::daemon
QTEST_GUILESS_MAIN(ketplus::daemon::ManagedTaskOriginHostTest)
#include "ManagedTaskOriginHostTest.moc"
