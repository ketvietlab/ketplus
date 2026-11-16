#include "agents/AgentInstructions.h"
#include "agents/AgentMcpServer.h"
#include "agents/AgentMutationOrigin.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

using namespace ketplus::daemon;
namespace {
QJsonObject invoke(const QString& name, const QJsonObject& arguments,
                   const agent_mcp::DaemonCall& call) {
    return agent_mcp::handleMessage(
        {{"jsonrpc", "2.0"},
         {"id", 7},
         {"method", "tools/call"},
         {"params", QJsonObject{{"name", name}, {"arguments", arguments}}}},
        call);
}
} // namespace

class AgentBackgroundToolsTest final : public QObject {
    Q_OBJECT
  private slots:
    void readOnlyBootstrapDoesNotAdvertiseMutation() {
        const auto response = agent_mcp::handleMessage(
            {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
            [](const QString&, const QJsonObject&) { return QJsonObject{{"ok", true}}; }, false);
        QSet<QString> names;
        for (const auto& tool : response.value("result").toObject().value("tools").toArray())
            names.insert(tool.toObject().value("name").toString());
        QVERIFY(!names.contains("background_task_start"));
        QVERIFY(!names.contains("background_task_stop"));
        for (const auto* name :
             {"background_task_list", "background_task_get", "background_task_output"})
            QVERIFY(names.contains(name));
    }

    void allProvidersReceiveOwnerBoundSchemas() {
        const auto response = agent_mcp::handleMessage(
            {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
            [](const QString&, const QJsonObject&) { return QJsonObject{{"ok", true}}; });
        const auto tools = response.value("result").toObject().value("tools").toArray();
        QSet<QString> names;
        for (const auto value : tools) {
            const auto tool = value.toObject();
            const auto name = tool.value("name").toString();
            names.insert(name);
            if (!name.startsWith("background_task_"))
                continue;
            const auto schema = tool.value("inputSchema").toObject();
            QVERIFY(!schema.value("additionalProperties").toBool(true));
            const auto properties = schema.value("properties").toObject();
            for (const auto key :
                 {"owner_session_id", "owner_generation", "session_id", "agent_token"})
                QVERIFY(!properties.contains(key));
            if (name == "background_task_start") {
                QVERIFY(schema.value("required").toArray().contains("client_request_id"));
                const auto execution = properties.value("execution_spec").toObject();
                QCOMPARE(execution.value("oneOf").toArray().size(), 2);
                QVERIFY(!execution.value("oneOf")
                             .toArray()[1]
                             .toObject()
                             .value("properties")
                             .toObject()
                             .contains("permissionMode"));
            }
        }
        for (const auto operation : {"start", "list", "get", "stop", "output"})
            QVERIFY(names.contains(QStringLiteral("background_task_") + operation));
        QCOMPARE(names.size(), 11);
        QVERIFY(!names.contains("background_task_resolve_cleanup"));
        const auto instructions = agent_messaging::toolInstructions("4821");
        QVERIFY(instructions.contains("background_task_start"));
        QVERIFY(instructions.contains("do not poll"));
    }

    void forwardsExactSpecsCursorsAndErrorsWithoutInventingOwner() {
        const QJsonObject process{
            {"client_request_id", "stable-request"},
            {"kind", "process"},
            {"title", "Build"},
            {"timeout_ms", 5000},
            {"execution_spec", QJsonObject{{"argv", QJsonArray{"tool", "literal $argument"}},
                                           {"cwd", "/workspace"},
                                           {"env_delta", QJsonObject{}}}}};
        const QJsonObject child{{"client_request_id", "child-request"},
                                {"kind", "agent"},
                                {"title", "Inspect"},
                                {"timeout_ms", 5000},
                                {"execution_spec", QJsonObject{{"prompt", "Inspect the code"},
                                                               {"provider", "cursor"}}}};
        for (const auto& payload : {process, child}) {
            int calls = 0;
            const agent_mcp::DaemonCall daemon = [&](const QString& operation,
                                                     const QJsonObject& sent) {
                ++calls;
                if (operation != "session.task.start" || sent != payload)
                    return QJsonObject{{"ok", false}, {"error_code", "fixture_mismatch"}};
                return QJsonObject{
                    {"ok", true},
                    {"accepted", true},
                    {"task", QJsonObject{{"task_id", "same-id"}, {"state", "queued"}}}};
            };
            QCOMPARE(invoke("background_task_start", payload, daemon),
                     invoke("background_task_start", payload, daemon));
            QCOMPARE(calls, 2); // Retry forwards the same key; the registry owns deduplication.
        }
        for (const auto& operation : {QStringLiteral("list"), QStringLiteral("get"),
                                      QStringLiteral("stop"), QStringLiteral("output")}) {
            const QJsonObject payload{{"task_id", "job"}, {"cursor", "opaque-byte-cursor"}};
            QString observed;
            QJsonObject sent;
            const auto response = invoke("background_task_" + operation, payload,
                                         [&](const QString& method, const QJsonObject& value) {
                                             observed = method;
                                             sent = value;
                                             return QJsonObject{{"ok", false},
                                                                {"error_code", "permission_denied"},
                                                                {"error_message", "Owner denied"}};
                                         });
            QCOMPARE(observed, "session.task." + operation);
            QCOMPARE(sent, payload);
            QVERIFY(response.value("result").toObject().value("isError").toBool());
            QVERIFY(QJsonDocument(response).toJson().contains("permission_denied"));
        }
    }

    void cannotExposeLocalCleanupAcknowledgement() {
        int calls = 0;
        const auto response =
            invoke("background_task_resolve_cleanup", {}, [&](const QString&, const QJsonObject&) {
                ++calls;
                return QJsonObject{};
            });
        QCOMPARE(calls, 0);
        QCOMPARE(response.value("error").toObject().value("code").toInt(), -32602);
    }

    void invocationCredentialIsImmutableAndNeverTakenFromArguments() {
        QString bootstrap = "original-fixture-secret";
        QJsonObject sent;
        const auto call = agent_mcp::withMutationCredential(
            [&](const QString&, const QJsonObject& payload) {
                sent = payload;
                return QJsonObject{};
            },
            bootstrap);
        bootstrap = "next-turn-fixture-secret";
        for (const auto* operation : {"session.task.start", "session.task.stop"}) {
            call(operation, {{mutationOriginField, "spoofed"}, {"task_id", "fixture"}});
            QCOMPARE(sent.value(mutationOriginField), "original-fixture-secret");
        }
        call("session.task.output", {{mutationOriginField, "spoofed"}});
        QVERIFY(!sent.contains(mutationOriginField));
        const auto review = agent_mcp::withMutationCredential(
            [&](const QString&, const QJsonObject& payload) {
                sent = payload;
                return QJsonObject{};
            },
            {});
        review("session.task.start", {{mutationOriginField, "normal-turn-secret"}});
        QVERIFY(sent.value(mutationOriginField).toString().isEmpty());
    }
};

QTEST_GUILESS_MAIN(AgentBackgroundToolsTest)
#include "AgentBackgroundToolsTest.moc"
