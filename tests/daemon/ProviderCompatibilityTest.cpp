#include "providers/compat/ProviderCompatibility.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "providers/compat/ProviderCapabilities.h"
#include "providers/compat/ProviderVersionProbe.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include "FakeCodex.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <optional>

using namespace ketplus::daemon;

namespace {

const QJsonObject manifest{
    {"manifest_version", 1},
    {"providers",
     QJsonObject{{"codex", QJsonObject{{"name", "Codex CLI"},
                                       {"minimum", "0.142.1"},
                                       {"tested", QJsonArray{"0.150.0", "0.154.0"}},
                                       {"update", "npm install -g @openai/codex@latest"},
                                       {"update_homebrew", "brew upgrade codex"}}}}}};

QJsonObject request(const QString& type, const QString& id, const QJsonObject& payload = {}) {
    return {{"protocol_version", protocol::currentVersion},
            {"message_id", id},
            {"message_type", type},
            {"sent_at", protocol::timestamp()},
            {"payload", payload}};
}

QJsonArray eventsOfType(LocalJournal& journal, const QString& sessionId, const QString& type) {
    QJsonArray matches;
    for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0)) {
        if (value.toObject().value(QStringLiteral("event_type")).toString() == type)
            matches.append(value);
    }
    return matches;
}

QByteArray logText(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool writeFile(const QString& path, const QByteArray& content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

// A shell CLI that logs each call and prints `banner`.
bool writeVersionScript(const QString& path, const QString& logPath, const QString& banner) {
    const QByteArray source =
        QStringLiteral("#!/bin/sh\necho call >> '%1'\necho '%2'\n").arg(logPath, banner).toUtf8();
    return writeFile(path, source) &&
           QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

} // namespace

class ProviderCompatibilityTest final : public QObject {
    Q_OBJECT

  private slots:
    void parsesAndComparesVersions() {
        QCOMPARE(compat::parseVersion(QStringLiteral("codex-cli 0.154.0")).toString(),
                 QStringLiteral("0.154.0"));
        QCOMPARE(compat::parseVersion(QStringLiteral("2026.09.18-9a7762b")).toString(),
                 QStringLiteral("2026.9.18"));
        QVERIFY(!compat::parseVersion(QStringLiteral("no version here")).isValid());
        QCOMPARE(compat::compare(compat::parseVersion(QStringLiteral("1.2")),
                                 compat::parseVersion(QStringLiteral("1.2.0"))),
                 0);
        QVERIFY(compat::compare(compat::parseVersion(QStringLiteral("0.142.1")),
                                compat::parseVersion(QStringLiteral("0.142"))) > 0);
        QVERIFY(compat::compare(compat::parseVersion(QStringLiteral("0.99.0")),
                                compat::parseVersion(QStringLiteral("0.142.1"))) < 0);
    }

    void evaluatesSupportStatus() {
        const compat::Policy policy = compat::policyFrom(manifest, QStringLiteral("codex"));
        QVERIFY(policy.isValid());
        QCOMPARE(policy.newestTested.toString(), QStringLiteral("0.154.0"));
        const QString exe = QStringLiteral("/usr/local/bin/codex");

        const compat::Runtime supported =
            compat::evaluate(policy, exe, QStringLiteral("codex-cli 0.142.1"));
        QCOMPARE(supported.status, compat::SupportStatus::Supported);
        QVERIFY(supported.message.isEmpty());
        QVERIFY(!supported.blocksLaunch());

        const compat::Runtime newer = compat::evaluate(policy, exe, QStringLiteral("0.160.2"));
        QCOMPARE(newer.status, compat::SupportStatus::Newer);
        QVERIFY(newer.message.contains(QStringLiteral("0.154.0")));
        QVERIFY(!newer.blocksLaunch());

        const compat::Runtime outdated = compat::evaluate(policy, exe, QStringLiteral("0.120.0"));
        QCOMPARE(outdated.status, compat::SupportStatus::Outdated);
        QVERIFY(outdated.blocksLaunch());
        QVERIFY(outdated.message.contains(QStringLiteral("npm install -g @openai/codex@latest")));
        QVERIFY(outdated.message.contains(QStringLiteral("0.142.1")));

        const compat::Runtime unknown = compat::evaluate(policy, exe, QString());
        QCOMPARE(unknown.status, compat::SupportStatus::Unknown);
        QVERIFY(!unknown.blocksLaunch());

        const QJsonObject json = outdated.toJson();
        QCOMPARE(json.value("status").toString(), QStringLiteral("outdated"));
        QCOMPARE(json.value("version").toString(), QStringLiteral("0.120.0"));
        QCOMPARE(json.value("minimum").toString(), QStringLiteral("0.142.1"));
        QCOMPARE(json.value("newest_tested").toString(), QStringLiteral("0.154.0"));
    }

    void prefersHomebrewUpdateForHomebrewInstalls() {
        const compat::Policy policy = compat::policyFrom(manifest, QStringLiteral("codex"));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDir root(directory.path());
        QVERIFY(root.mkpath(QStringLiteral("Caskroom/codex/0.154.0")));
        QVERIFY(root.mkpath(QStringLiteral("lib/node_modules/@openai/codex/bin")));
        QVERIFY(root.mkpath(QStringLiteral("bin")));
        const QString cask = root.filePath(QStringLiteral("Caskroom/codex/0.154.0/codex"));
        const QString npm =
            root.filePath(QStringLiteral("lib/node_modules/@openai/codex/bin/codex.js"));
        QVERIFY(writeFile(cask, "#!/bin/sh\n"));
        QVERIFY(writeFile(npm, "#!/bin/sh\n"));
        const QString brewLink = root.filePath(QStringLiteral("bin/codex"));
        const QString npmLink = root.filePath(QStringLiteral("bin/codex-npm"));
        QVERIFY(QFile::link(cask, brewLink));
        QVERIFY(QFile::link(npm, npmLink));
        // The link in bin/ is resolved to where the package manager installed it.
        QCOMPARE(policy.updateCommandFor(brewLink), QStringLiteral("brew upgrade codex"));
        QCOMPARE(policy.updateCommandFor(npmLink),
                 QStringLiteral("npm install -g @openai/codex@latest"));
    }

    void builtInManifestCoversUserInstalledClis() {
        const compat::Policy codex = compat::policy(QStringLiteral("codex"));
        const compat::Policy cursor = compat::policy(QStringLiteral("cursor"));
        QVERIFY(codex.isValid());
        QVERIFY(cursor.isValid());
        QVERIFY(compat::compare(codex.minimum, codex.newestTested) <= 0);
        QVERIFY(compat::compare(cursor.minimum, cursor.newestTested) <= 0);
        QVERIFY(!codex.updateCommand.isEmpty());
        QVERIFY(!cursor.updateCommand.isEmpty());
        // Claude is bundled, so it has no user-installed range.
        QVERIFY(!compat::policy(QStringLiteral("claude")).isValid());
    }

    void readsBundledClaudeVersions() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString entry = directory.filePath(QStringLiteral("index.mjs"));
        QVERIFY(writeFile(directory.filePath(QStringLiteral("runtime.json")),
                          R"({"sdkVersion":"0.3.1","claudeCodeVersion":"2.1.1"})"));
        compat::Runtime staged = compat::bundledClaude(entry);
        QCOMPARE(staged.status, compat::SupportStatus::Bundled);
        QCOMPARE(staged.version, QStringLiteral("0.3.1"));
        QCOMPARE(staged.components.value("claude_code").toString(), QStringLiteral("2.1.1"));

        QTemporaryDir checkout;
        QVERIFY(checkout.isValid());
        QVERIFY(QDir(checkout.path())
                    .mkpath(QStringLiteral("node_modules/@anthropic-ai/claude-agent-sdk")));
        QVERIFY(writeFile(checkout.filePath(QStringLiteral(
                              "node_modules/@anthropic-ai/claude-agent-sdk/package.json")),
                          R"({"version":"0.3.2","claudeCodeVersion":"2.1.2"})"));
        const compat::Runtime source =
            compat::bundledClaude(checkout.filePath(QStringLiteral("index.mjs")));
        QCOMPARE(source.components.value("sdk").toString(), QStringLiteral("0.3.2"));
        QCOMPARE(source.components.value("claude_code").toString(), QStringLiteral("2.1.2"));

        QCOMPARE(compat::bundledClaude({}).status, compat::SupportStatus::Missing);
    }

    void probeCachesUntilTheExecutableChanges() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cli = directory.filePath(QStringLiteral("codex"));
        const QString logPath = directory.filePath(QStringLiteral("calls.log"));
        QVERIFY(writeVersionScript(cli, logPath, QStringLiteral("codex-cli 0.150.0")));

        compat::ProviderVersionProbe probe(compat::policyFrom(manifest, QStringLiteral("codex")));
        std::optional<compat::Runtime> first;
        std::optional<compat::Runtime> joined;
        probe.check(cli, [&first](const compat::Runtime& runtime) { first = runtime; });
        // A second launch while the probe runs waits for the same process.
        probe.check(cli, [&joined](const compat::Runtime& runtime) { joined = runtime; });
        QTRY_VERIFY(first.has_value() && joined.has_value());
        QCOMPARE(first->status, compat::SupportStatus::Supported);
        QCOMPARE(first->version, QStringLiteral("0.150.0"));
        QCOMPARE(logText(logPath).count("call"), 1);

        bool answeredSynchronously = false;
        probe.check(cli, [&answeredSynchronously](const compat::Runtime&) {
            answeredSynchronously = true;
        });
        QVERIFY(answeredSynchronously);
        QCOMPARE(logText(logPath).count("call"), 1);

        // An update on disk (new size and time) is noticed at the next check.
        QVERIFY(writeVersionScript(cli, logPath, QStringLiteral("codex-cli 0.120.10")));
        std::optional<compat::Runtime> updated;
        probe.check(cli, [&updated](const compat::Runtime& runtime) { updated = runtime; });
        QTRY_VERIFY(updated.has_value());
        QCOMPARE(updated->status, compat::SupportStatus::Outdated);
        QCOMPARE(logText(logPath).count("call"), 2);

        std::optional<compat::Runtime> gone;
        probe.check(directory.filePath(QStringLiteral("missing")),
                    [&gone](const compat::Runtime& runtime) { gone = runtime; });
        QTRY_VERIFY(gone.has_value());
        QCOMPARE(gone->status, compat::SupportStatus::Missing);
    }

    void selectsTheDialectForEachVersion() {
        const QJsonObject dialects{
            {"providers",
             QJsonObject{
                 {"codex",
                  QJsonObject{
                      {"minimum", "0.142.1"},
                      {"tested", QJsonArray{"0.154.0"}},
                      {"dialects",
                       QJsonArray{QJsonObject{{"id", "v3"},
                                              {"from", "0.150.0"},
                                              {"capabilities", QJsonArray{"turnSteer", "images"}}},
                                  QJsonObject{{"id", "v2"},
                                              {"from", "0.142.1"},
                                              {"capabilities", QJsonArray{"images"}}}}}}},
                 {"claude", QJsonObject{{"name", "Claude Agent SDK"},
                                        {"bundled", true},
                                        {"capabilities", QJsonArray{"turnSteer"}}}}}}};
        const compat::Policy codex = compat::policyFrom(dialects, QStringLiteral("codex"));
        QCOMPARE(codex.dialects.size(), 2);
        QCOMPARE(codex.dialects.first().id, QStringLiteral("v2"));
        QCOMPARE(codex.dialectFor(compat::parseVersion(QStringLiteral("0.143.0")))->id,
                 QStringLiteral("v2"));
        QCOMPARE(codex.dialectFor(compat::parseVersion(QStringLiteral("0.150.0")))->id,
                 QStringLiteral("v3"));
        QCOMPARE(codex.dialectFor({})->id, QStringLiteral("v3"));
        QVERIFY(codex.dialectFor(compat::parseVersion(QStringLiteral("0.1.0"))) == nullptr);
        QVERIFY(codex.baselineSupports(QStringLiteral("turnSteer")));

        const compat::Runtime older =
            compat::evaluate(codex, QStringLiteral("/bin/codex"), QStringLiteral("0.145.0"));
        QCOMPARE(older.dialect, QStringLiteral("v2"));
        QVERIFY(older.supports(QStringLiteral("images")));
        QVERIFY(!older.supports(QStringLiteral("turnSteer")));
        const QJsonObject json = older.toJson();
        QCOMPARE(json.value(QStringLiteral("dialect")).toString(), QStringLiteral("v2"));
        QCOMPARE(json.value(QStringLiteral("capabilities")).toArray(), QJsonArray{"images"});
        // Outdated CLIs never run, so they advertise nothing.
        const compat::Runtime outdated =
            compat::evaluate(codex, QStringLiteral("/bin/codex"), QStringLiteral("0.100.0"));
        QVERIFY(outdated.dialect.isEmpty());
        QVERIFY(outdated.capabilities.isEmpty());

        const compat::Policy claude = compat::policyFrom(dialects, QStringLiteral("claude"));
        QVERIFY(claude.bundled);
        QVERIFY(claude.baselineSupports(QStringLiteral("turnSteer")));
        QVERIFY(!claude.baselineSupports(QStringLiteral("threadCompact")));
        const compat::Runtime bundled =
            compat::bundledClaude(QStringLiteral("/x/main.mjs"), claude);
        QCOMPARE(bundled.dialect, QStringLiteral("bundled"));
        QCOMPARE(bundled.capabilities, QStringList{QStringLiteral("turnSteer")});
    }

    void builtInManifestDeclaresCapabilities() {
        for (const QString provider : {QStringLiteral("codex"), QStringLiteral("cursor")}) {
            const compat::Policy policy = compat::policy(provider);
            QVERIFY2(!policy.dialects.isEmpty(), qPrintable(provider));
            // The oldest supported version must map to a dialect.
            QVERIFY2(policy.dialectFor(policy.minimum) != nullptr, qPrintable(provider));
        }
        QVERIFY(compat::policy(QStringLiteral("codex"))
                    .baselineSupports(compat::capability::turnSteer));
        QVERIFY(!compat::policy(QStringLiteral("cursor"))
                     .baselineSupports(compat::capability::turnSteer));
        // The daemon does not implement compaction for any provider yet.
        for (const QString provider :
             {QStringLiteral("codex"), QStringLiteral("cursor"), QStringLiteral("claude")})
            QVERIFY(!compat::policy(provider).baselineSupports(compat::capability::threadCompact));
    }

    void narrowsCapabilitiesAtRuntime() {
        QCOMPARE(compat::capabilityForMethod(u"turn/steer"), QStringLiteral("turnSteer"));
        QCOMPARE(compat::capabilityForMethod(u"session/load"), QStringLiteral("threadResume"));
        QVERIFY(compat::capabilityForMethod(u"turn/start").isEmpty());

        compat::CapabilityNarrowing narrowing;
        QCOMPARE(narrowing.methodNotFound(u"turn/steer"), QStringLiteral("turnSteer"));
        QVERIFY(narrowing.methodNotFound(u"turn/start").isEmpty());
        QVERIFY(narrowing.removed(QStringLiteral("turnSteer")));
        compat::Runtime runtime;
        runtime.capabilities = {QStringLiteral("images"), QStringLiteral("turnSteer")};
        QCOMPARE(narrowing.apply(runtime).capabilities, QStringList{QStringLiteral("images")});

        // ACP omits defaults, so only explicit false narrows.
        narrowing.reset();
        narrowing.acpInitialized({{"agentCapabilities", QJsonObject{}}});
        QVERIFY(!narrowing.removed(QStringLiteral("threadResume")));
        narrowing.acpInitialized(
            {{"agentCapabilities",
              QJsonObject{{"loadSession", false},
                          {"promptCapabilities", QJsonObject{{"image", false}}}}}});
        QVERIFY(narrowing.removed(QStringLiteral("threadResume")));
        QVERIFY(narrowing.removed(QStringLiteral("images")));
    }

    void methodNotFoundNarrowsCodexSteering() {
        qputenv("KETPLUS_FAKE_CODEX_UNSUPPORTED", "turn/steer");
        const auto restore = qScopeGuard([] { qunsetenv("KETPLUS_FAKE_CODEX_UNSUPPORTED"); });
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(executable, directory.filePath(QStringLiteral("provider.log"))));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto stopProvider = qScopeGuard([&adapter] { adapter.stop(); });

        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("start-narrow"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"), QStringLiteral("Hello")}}),
            QStringLiteral("desktop-test"));
        QVERIFY(started.value(QStringLiteral("ok")).toBool());
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        const qint64 generation = started.value(QStringLiteral("session_generation")).toInteger();
        QTRY_COMPARE_WITH_TIMEOUT(
            eventsOfType(journal, sessionId, QStringLiteral("turn.completed")).size(), 1, 10000);
        QVERIFY(host.providerSupports(QStringLiteral("codex"), compat::capability::turnSteer));

        std::optional<QString> steerError;
        adapter.call(
            QStringLiteral("turn/steer"), {},
            [&steerError](const QJsonObject&, const QString& error) { steerError = error; });
        QTRY_VERIFY(steerError.has_value());
        QVERIFY(!steerError->isEmpty());
        QVERIFY(!adapter.supports(compat::capability::turnSteer));
        QVERIFY(adapter.supports(compat::capability::images));

        QJsonObject steer = request(QStringLiteral("session.steer"), QStringLiteral("steer"),
                                    {{QStringLiteral("prompt"), QStringLiteral("Faster")}});
        steer.insert(QStringLiteral("session_id"), sessionId);
        steer.insert(QStringLiteral("session_generation"), generation);
        const QJsonObject refused = host.handleCommand(steer, QStringLiteral("desktop-test"));
        QCOMPARE(refused.value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("unsupported_provider_operation"));

        std::optional<QJsonObject> answer;
        host.handleProviderQuery(
            request(QStringLiteral("provider.runtime_status"), QStringLiteral("narrowed")),
            [&answer](const QJsonObject& result, const QString&) { answer = result; });
        QTRY_VERIFY(answer.has_value());
        const QJsonArray capabilities = answer->value(QStringLiteral("providers"))
                                            .toArray()
                                            .first()
                                            .toObject()
                                            .value(QStringLiteral("capabilities"))
                                            .toArray();
        QVERIFY(!capabilities.contains(QStringLiteral("turnSteer")));
        QVERIFY(capabilities.contains(QStringLiteral("images")));
    }

    void oversizedCodexMessagesBecomeNoticesAndTheTurnEnds() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(executable, directory.filePath(QStringLiteral("provider.log"))));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto stopProvider = qScopeGuard([&adapter] { adapter.stop(); });

        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("start-oversized"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"), QStringLiteral("oversized")}}),
            QStringLiteral("desktop-test"));
        QVERIFY(started.value(QStringLiteral("ok")).toBool());
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        QTRY_COMPARE_WITH_TIMEOUT(
            eventsOfType(journal, sessionId, QStringLiteral("turn.completed")).size(), 1, 10000);
        const QJsonArray errors =
            eventsOfType(journal, sessionId, QStringLiteral("provider.protocol_error"));
        QCOMPARE(errors.size(), 2);
        QCOMPARE(errors.first()["payload"]["method"].toString(), "item/completed");
        QVERIFY(errors.first()["payload"]["oversized_bytes"].toInteger() > 16 * 1024 * 1024);
        QCOMPARE(errors.last()["payload"]["method"].toString(), "turn/completed");
        QVERIFY(eventsOfType(journal, sessionId, QStringLiteral("runtime.error")).isEmpty());
    }

    void unknownCodexInputIsHandledSafely() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        const QString providerLog = directory.filePath(QStringLiteral("provider.log"));
        QVERIFY(writeFakeCodex(executable, providerLog));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto stopProvider = qScopeGuard([&adapter] { adapter.stop(); });

        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("start-unknown"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"), QStringLiteral("unknown-input")}}),
            QStringLiteral("desktop-test"));
        QVERIFY(started.value(QStringLiteral("ok")).toBool());
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        QTRY_COMPARE_WITH_TIMEOUT(
            eventsOfType(journal, sessionId, QStringLiteral("turn.completed")).size(), 1, 10000);

        // An unknown server request is declined, and the turn says so.
        QVERIFY(logText(providerLog).contains("response:future:"));
        const QJsonArray declined =
            eventsOfType(journal, sessionId, QStringLiteral("provider.request_declined"));
        QCOMPARE(declined.size(), 1);
        QCOMPARE(declined.first()["payload"]["method"].toString(), "item/future/request");

        // A known message missing a required field is one protocol error, not an empty row.
        const QJsonArray errors =
            eventsOfType(journal, sessionId, QStringLiteral("provider.protocol_error"));
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first()["payload"]["method"].toString(), "item/completed");
        QCOMPARE(errors.first()["payload"]["missing"].toArray(), QJsonArray{"item.id"});
        const QJsonArray items = eventsOfType(journal, sessionId, QStringLiteral("item.completed"));
        for (const QJsonValue& item : items)
            QVERIFY(!item["payload"]["item_id"].toString().isEmpty());

        // An unknown item type is kept with its type name.
        QVERIFY(std::any_of(items.begin(), items.end(), [](const QJsonValue& item) {
            return item["payload"]["item_type"].toString() == "futureItem";
        }));

        // Unknown notifications are counted, not journaled.
        std::optional<QJsonObject> answer;
        host.handleProviderQuery(
            request(QStringLiteral("provider.runtime_status"), QStringLiteral("ignored")),
            [&answer](const QJsonObject& result, const QString&) { answer = result; });
        QTRY_VERIFY(answer.has_value());
        const QJsonObject codex =
            answer->value(QStringLiteral("providers")).toArray().first().toObject();
        QCOMPARE(codex["ignored_notifications"]["thread/futureSignal"].toInteger(), 2);
    }

    void outdatedCodexFailsTheTurnWithoutStarting() {
        qputenv("KETPLUS_FAKE_CODEX_VERSION", "0.100.0");
        const auto restore = qScopeGuard([] { qunsetenv("KETPLUS_FAKE_CODEX_VERSION"); });
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        const QString providerLog = directory.filePath(QStringLiteral("provider.log"));
        QVERIFY(writeFakeCodex(executable, providerLog));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto stopProvider = qScopeGuard([&adapter] { adapter.stop(); });

        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("start-outdated"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"), QStringLiteral("Hello")}}),
            QStringLiteral("desktop-test"));
        QVERIFY(started.value(QStringLiteral("ok")).toBool());
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();

        QTRY_COMPARE(eventsOfType(journal, sessionId, QStringLiteral("runtime.error")).size(), 1);
        const QString message = eventsOfType(journal, sessionId, QStringLiteral("runtime.error"))
                                    .first()
                                    .toObject()
                                    .value(QStringLiteral("payload"))
                                    .toObject()
                                    .value(QStringLiteral("message"))
                                    .toString();
        QVERIFY2(message.contains(QStringLiteral("0.100.0")), qPrintable(message));
        QVERIFY2(message.contains(QStringLiteral("0.142.1")), qPrintable(message));
        // The provider was never launched as an app-server.
        QVERIFY(!logText(providerLog).contains("args:"));
        QVERIFY(eventsOfType(journal, sessionId, QStringLiteral("provider.runtime")).isEmpty());
    }

    void untestedCodexRunsAndIsJournaledOnce() {
        qputenv("KETPLUS_FAKE_CODEX_VERSION", "9.0.0");
        const auto restore = qScopeGuard([] { qunsetenv("KETPLUS_FAKE_CODEX_VERSION"); });
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        const QString providerLog = directory.filePath(QStringLiteral("provider.log"));
        QVERIFY(writeFakeCodex(executable, providerLog));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto stopProvider = qScopeGuard([&adapter] { adapter.stop(); });

        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("start-newer"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"), QStringLiteral("Hello")}}),
            QStringLiteral("desktop-test"));
        QVERIFY(started.value(QStringLiteral("ok")).toBool());
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        const qint64 generation = started.value(QStringLiteral("session_generation")).toInteger();
        QTRY_COMPARE_WITH_TIMEOUT(
            eventsOfType(journal, sessionId, QStringLiteral("turn.completed")).size(), 1, 10000);
        QCOMPARE(eventsOfType(journal, sessionId, QStringLiteral("provider.runtime")).size(), 1);
        const QJsonObject runtime =
            eventsOfType(journal, sessionId, QStringLiteral("provider.runtime"))
                .first()
                .toObject()
                .value(QStringLiteral("payload"))
                .toObject();
        QCOMPARE(runtime.value(QStringLiteral("status")).toString(), QStringLiteral("newer"));
        QCOMPARE(runtime.value(QStringLiteral("version")).toString(), QStringLiteral("9.0.0"));

        QJsonObject next =
            request(QStringLiteral("session.start_turn"), QStringLiteral("start-newer-again"),
                    {{QStringLiteral("prompt"), QStringLiteral("Again")}});
        next.insert(QStringLiteral("session_id"), sessionId);
        next.insert(QStringLiteral("session_generation"), generation);
        QVERIFY(host.handleCommand(next, QStringLiteral("desktop-test"))
                    .value(QStringLiteral("ok"))
                    .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(
            eventsOfType(journal, sessionId, QStringLiteral("turn.completed")).size(), 2, 10000);
        QCOMPARE(eventsOfType(journal, sessionId, QStringLiteral("provider.runtime")).size(), 1);
    }

    void reportsEveryProviderRuntime() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(executable, directory.filePath(QStringLiteral("provider.log"))));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);

        std::optional<QJsonObject> answer;
        host.handleProviderQuery(
            request(QStringLiteral("provider.runtime_status"), QStringLiteral("runtimes")),
            [&answer](const QJsonObject& result, const QString& error) {
                QVERIFY(error.isEmpty());
                answer = result;
            });
        QTRY_VERIFY(answer.has_value());
        QVERIFY(answer->value(QStringLiteral("ok")).toBool());
        const QJsonArray providers = answer->value(QStringLiteral("providers")).toArray();
        QCOMPARE(providers.size(), 3);
        const QJsonObject codex = providers.at(0).toObject();
        QCOMPARE(codex.value(QStringLiteral("provider")).toString(), QStringLiteral("codex"));
        QCOMPARE(codex.value(QStringLiteral("status")).toString(), QStringLiteral("supported"));
        QCOMPARE(codex.value(QStringLiteral("version")).toString(), QStringLiteral("0.154.0"));
        QCOMPARE(codex.value(QStringLiteral("dialect")).toString(),
                 QStringLiteral("codex-app-server-v2"));
        QVERIFY(codex.value(QStringLiteral("capabilities"))
                    .toArray()
                    .contains(QStringLiteral("turnSteer")));
        // No sidecar in this host: a missing runtime advertises nothing.
        QVERIFY(
            providers.at(1).toObject().value(QStringLiteral("capabilities")).toArray().isEmpty());
        QCOMPARE(providers.at(1).toObject().value(QStringLiteral("provider")).toString(),
                 QStringLiteral("claude"));
        const QJsonObject cursor = providers.at(2).toObject();
        QCOMPARE(cursor.value(QStringLiteral("provider")).toString(), QStringLiteral("cursor"));
        QCOMPARE(cursor.value(QStringLiteral("status")).toString(), QStringLiteral("missing"));
    }
};

QTEST_GUILESS_MAIN(ProviderCompatibilityTest)
#include "ProviderCompatibilityTest.moc"
