#include "runtime/ManagedTaskSpec.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;

namespace {
ManagedTaskContext context(const QString& cwd) {
    return {{"authenticated-owner", 7, "turn"},
            cwd,
            "fake-provider",
            "model-a",
            "normal",
            [cwd](const QString& path) { return path == cwd || path.startsWith(cwd + '/'); },
            [](const QString& provider, const QString& model, const QString& effort) {
                return provider == "fake-provider" && (model == "model-a" || model == "model-b") &&
                       effort == "normal";
            }};
}
QJsonObject payload(const QString& cwd) {
    return {{"client_request_id", "request"},
            {"kind", "process"},
            {"title", "Fixture"},
            {"timeout_ms", 1000},
            {"execution_spec", QJsonObject{{"argv", QJsonArray{"/bin/echo", "hello"}},
                                           {"cwd", cwd},
                                           {"env_delta", QJsonObject{}}}}};
}
} // namespace

class ManagedTaskSpecTest final : public QObject {
    Q_OBJECT
  private slots:
    void authenticatedOwnerAndCredentialFreeMetadata() {
        QTemporaryDir directory;
        const auto cwd = QFileInfo(directory.path()).canonicalFilePath();
        auto input = payload(cwd);
        auto spec = input.value("execution_spec").toObject();
        spec.insert("env_delta", QJsonObject{{"TEST_TOKEN", "fixture-secret-value"}});
        spec.insert("argv", QJsonArray{"/bin/echo", "fixture-secret-argument"});
        input.insert("execution_spec", spec);
        const auto admitted = validateManagedTask(input, context(cwd));
        QVERIFY2(admitted.error.isEmpty(), qPrintable(admitted.error));
        QCOMPARE(admitted.intent.owner.sessionId, "authenticated-owner");
        QCOMPARE(admitted.intent.owner.generation, 7);
        QCOMPARE(admitted.execution.value("env_delta").toObject().value("TEST_TOKEN"),
                 "fixture-secret-value");
        const auto metadata = QJsonDocument(admitted.intent.executionMetadata).toJson();
        QVERIFY(!metadata.contains("fixture-secret"));
        QCOMPARE(admitted.intent.specFingerprint.size(), 32);
        input.insert("owner_session_id", "victim");
        QCOMPARE(validateManagedTask(input, context(cwd)).errorCode, "invalid_request");
    }

    void typedProcessSpecAndCanonicalWorkspaceFence() {
        QTemporaryDir directory;
        QTemporaryDir outside;
        const auto cwd = QFileInfo(directory.path()).canonicalFilePath();
        auto input = payload(cwd);
        auto spec = input.value("execution_spec").toObject();
        spec.insert("explicit_shell_command", "echo hello");
        input.insert("execution_spec", spec);
        QCOMPARE(validateManagedTask(input, context(cwd)).errorCode, "invalid_execution");
        spec.remove("argv");
        input.insert("execution_spec", spec);
        const auto shell = validateManagedTask(input, context(cwd));
        QVERIFY(shell.error.isEmpty());
        QCOMPARE(shell.execution.value("shell_command"), "echo hello");
        QVERIFY(!shell.execution.contains("explicit_shell_command"));
        QVERIFY(QFile::link(outside.path(), cwd + "/escape"));
        spec.insert("cwd", cwd + "/escape");
        input.insert("execution_spec", spec);
        QCOMPARE(validateManagedTask(input, context(cwd)).errorCode, "cwd_forbidden");
        input = payload(cwd);
        input.insert("timeout_ms", 0.5);
        QCOMPARE(validateManagedTask(input, context(cwd)).errorCode, "invalid_request");
    }

    void childDefaultsSnapshotWithoutCallerPrivilegeOverrides() {
        QTemporaryDir directory;
        const auto cwd = QFileInfo(directory.path()).canonicalFilePath();
        auto input = payload(cwd);
        input.insert("kind", "agent");
        input.insert("execution_spec", QJsonObject{{"prompt", "Perform fixture work"}});
        auto owner = context(cwd);
        const auto original = validateManagedTask(input, owner);
        QVERIFY(original.error.isEmpty());
        QCOMPARE(original.execution.value("provider"), "fake-provider");
        QCOMPARE(original.intent.executionMetadata.value("model"), "model-a");
        owner.model = "model-b";
        const auto changedDefaults = validateManagedTask(input, owner);
        QCOMPARE(changedDefaults.intent.specFingerprint, original.intent.specFingerprint);
        QCOMPARE(changedDefaults.intent.executionMetadata.value("model"), "model-b");
        // The start handler uses the original fingerprint to return the accepted
        // record before consulting changed defaults/catalog on a retry.
        for (const auto& forbidden :
             {"session_id", "token", "env", "permissionMode", "system_prompt_override"}) {
            auto spec = input.value("execution_spec").toObject();
            spec.insert(QLatin1String(forbidden), "untrusted-override");
            input.insert("execution_spec", spec);
            QCOMPARE(validateManagedTask(input, owner).errorCode, "invalid_execution");
            spec.remove(QLatin1String(forbidden));
            input.insert("execution_spec", spec);
        }
    }
};
QTEST_GUILESS_MAIN(ManagedTaskSpecTest)
#include "ManagedTaskSpecTest.moc"
