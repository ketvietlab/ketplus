#include "providers/ClaudeAdapter.h"
#include "protocol/DaemonProtocol.h"
#include "providers/ClaudeAccountUsage.h"
#include "providers/CodexAdapter.h"
#include "runtime/ProviderHandoff.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include "FakeClaude.h"
#include "FakeCodex.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace ketplus::daemon;

namespace {

QJsonObject request(const QString& type, const QString& id, const QJsonObject& payload = {},
                    const QString& sessionId = {}, const qint64 generation = -1) {
    QJsonObject result{{"protocol_version", protocol::currentVersion},
                       {"message_id", id},
                       {"message_type", type},
                       {"sent_at", protocol::timestamp()},
                       {"payload", payload}};
    if (!sessionId.isEmpty()) {
        result.insert("session_id", sessionId);
    }
    if (generation >= 0) {
        result.insert("session_generation", generation);
    }
    return result;
}

QString startClaude(SessionHost& host, const QString& worktree, const QString& prompt) {
    const QJsonObject started = host.handleCommand(
        request(QStringLiteral("session.start_turn"), QStringLiteral("start-") + prompt,
                {{QStringLiteral("worktree_path"), worktree},
                 {QStringLiteral("prompt"), prompt},
                 {QStringLiteral("options"),
                  QJsonObject{{QStringLiteral("provider"), QStringLiteral("claude")},
                              {QStringLiteral("model"), QStringLiteral("sonnet")},
                              {QStringLiteral("permission"), QStringLiteral("workspace-write")}}}}),
        QStringLiteral("desktop-test"));
    return started.value(QStringLiteral("session_id")).toString();
}

auto eventsOf(LocalJournal& journal, const QString& sessionId) {
    return [&journal, sessionId](const QString& type) {
        QJsonArray matches;
        for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0)) {
            if (value.toObject().value(QStringLiteral("event_type")).toString() == type)
                matches.append(value);
        }
        return matches;
    };
}

QByteArray logText(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

} // namespace

class ClaudeAdapterTest final : public QObject {
    Q_OBJECT

  private slots:
    void remoteAccountUsageIsReadOnlyAndSessionFenced() {
        QTemporaryDir directory;
        const auto fake = directory.filePath("fake-claude");
        QVERIFY(writeFakeClaude(fake, directory.filePath("log")));
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        CodexAdapter codex("/missing-codex");
        ClaudeAdapter claude(fake);
        SessionHost host(&journal, &codex, &claude);
        int reads = 0;
        int writes = 0;
        ClaudeAccountUsage usage({[&](QString*) {
                                      ++reads;
                                      return QByteArray{};
                                  },
                                  [&](const QByteArray&, QString*) {
                                      ++writes;
                                      return true;
                                  },
                                  "fake"});
        host.setClaudeAccountUsage(&usage);
        const auto id = startClaude(host, directory.path(), "usage-read");
        QVERIFY(!id.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!eventsOf(journal, id)("turn.completed").isEmpty(), 3000);
        const QJsonObject payload{{"effective_only", true},
                                  {"include_claude_usage", true},
                                  {"resource_context_revision", 1},
                                  {"operation", "save"},
                                  {"key", "sk-ant-admin01-must-not-be-stored"}};
        auto read = request("session.configuration.read", "read", payload, id, 1);
        QJsonObject result;
        host.handleCommandDeferred(read, "cloud-control-v2", [&](const auto& r) { result = r; });
        QVERIFY(result.value("ok").toBool());
        const auto account = result.value("claude_account_usage").toObject();
        QVERIFY(account.value("supported").toBool());
        QVERIFY(!account.value("configured").toBool());
        QVERIFY(!QJsonDocument(result).toJson().contains("sk-ant-admin"));
        QCOMPARE(reads, 1);
        QCOMPARE(writes, 0);
        read.insert("session_generation", 999);
        host.handleCommandDeferred(read, "cloud-control-v2", [&](const auto& r) { result = r; });
        QVERIFY(!result.value("ok").toBool());
        QVERIFY(!result.contains("claude_account_usage"));
        QCOMPARE(reads, 1);
        read.insert("message_type", "provider.claude.credentials.analytics.save");
        host.handleCommandDeferred(read, "cloud-control-v2", [&](const auto& r) { result = r; });
        QVERIFY(!result.value("ok").toBool());
        QCOMPARE(writes, 0);
        for (const auto& event : journal.eventsAfter(id, 1, 0))
            QVERIFY(!event.toObject().value("payload").toObject().contains("claude_account_usage"));
    }
    void startsClaudeTurnAndJournalsCanonicalEvents() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString claude = directory.filePath(QStringLiteral("fake-claude"));
        const QString logPath = directory.filePath(QStringLiteral("claude.log"));
        QVERIFY(writeFakeClaude(claude, logPath));

        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-claude-test"));
        ClaudeAdapter adapter(claude);
        SessionHost host(&journal, &codex, &adapter);

        const QJsonObject started = host.handleCommand(
            request(
                QStringLiteral("session.start_turn"), QStringLiteral("start-claude"),
                {{QStringLiteral("worktree_path"), directory.path()},
                 {QStringLiteral("prompt"), QStringLiteral("Inspect the tree")},
                 {QStringLiteral("options"),
                  QJsonObject{{QStringLiteral("provider"), QStringLiteral("claude")},
                              {QStringLiteral("model"), QStringLiteral("sonnet")},
                              {QStringLiteral("permission"), QStringLiteral("workspace-write")}}}}),
            QStringLiteral("desktop-test"));
        QVERIFY2(started.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(started).toJson(QJsonDocument::Compact)));
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        QVERIFY(!sessionId.isEmpty());

        const auto events = [&journal, sessionId](const QString& type) {
            QJsonArray matches;
            for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0)) {
                if (value.toObject().value(QStringLiteral("event_type")).toString() == type)
                    matches.append(value);
            }
            return matches;
        };
        QTRY_VERIFY_WITH_TIMEOUT(!events(QStringLiteral("assistant.delta")).isEmpty(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!events(QStringLiteral("usage.context")).isEmpty(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!events(QStringLiteral("turn.completed")).isEmpty(), 3000);
        const QJsonObject claudeUsage = events(QStringLiteral("usage.context"))
                                            .first()
                                            .toObject()
                                            .value(QStringLiteral("payload"))
                                            .toObject()
                                            .value(QStringLiteral("tokenUsage"))
                                            .toObject();
        QCOMPARE(claudeUsage.value(QStringLiteral("last"))
                     .toObject()
                     .value(QStringLiteral("totalTokens"))
                     .toInteger(),
                 85000);
        QCOMPARE(claudeUsage.value(QStringLiteral("modelContextWindow")).toInteger(), 200000);
        // Plan rows come from a provider sidecar, so the journal keeps only rows it can
        // render: named, with a percentage, and clamped to the scale the view draws.
        QTRY_VERIFY_WITH_TIMEOUT(!events(QStringLiteral("usage.session")).isEmpty(), 3000);
        const auto accounting =
            events(QStringLiteral("usage.session")).first().toObject().value("payload").toObject();
        QCOMPARE(accounting.value("estimatedCostUsd").toDouble(), 12.5);
        QVERIFY(!accounting.contains("raw_private"));
        QVERIFY(!accounting.contains("outputTokens"));
        QTRY_VERIFY_WITH_TIMEOUT(!events(QStringLiteral("usage.limits")).isEmpty(), 3000);
        const QJsonArray planLimits = events(QStringLiteral("usage.limits"))
                                          .first()
                                          .toObject()
                                          .value(QStringLiteral("payload"))
                                          .toObject()
                                          .value(QStringLiteral("limits"))
                                          .toArray();
        QCOMPARE(planLimits.size(), 2);
        QCOMPARE(planLimits.first().toObject().value(QStringLiteral("id")).toString(),
                 QStringLiteral("five_hour"));
        QCOMPARE(planLimits.first().toObject().value(QStringLiteral("usedPercent")).toDouble(),
                 41.5);
        QCOMPARE(planLimits.last().toObject().value(QStringLiteral("usedPercent")).toDouble(),
                 100.0);
        QCOMPARE(events(QStringLiteral("session.created"))
                     .first()
                     .toObject()
                     .value(QStringLiteral("payload"))
                     .toObject()
                     .value(QStringLiteral("adapter"))
                     .toString(),
                 QStringLiteral("claude"));
        QCOMPARE(events(QStringLiteral("assistant.delta"))
                     .first()
                     .toObject()
                     .value(QStringLiteral("payload"))
                     .toObject()
                     .value(QStringLiteral("delta"))
                     .toString(),
                 QStringLiteral("finished"));

        const QJsonObject listed = host.handleCommand(
            request(QStringLiteral("session.list"), QStringLiteral("list-claude")),
            QStringLiteral("desktop-test"));
        QCOMPARE(listed.value(QStringLiteral("sessions"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("adapter"))
                     .toString(),
                 QStringLiteral("claude"));

        bool catalogHasClaude = false;
        bool catalogHasOpus46 = false;
        bool catalogDone = false;
        host.handleProviderQuery(
            request(QStringLiteral("provider.models"), QStringLiteral("models")),
            [&](const QJsonObject& result, const QString& error) {
                QVERIFY(error.isEmpty());
                for (const QJsonValue& value : result.value(QStringLiteral("data")).toArray()) {
                    const QJsonObject model = value.toObject();
                    if (model.value(QStringLiteral("provider")).toString() !=
                        QStringLiteral("claude"))
                        continue;
                    catalogHasClaude = true;
                    if (model.value(QStringLiteral("model")).toString() ==
                        QStringLiteral("claude-opus-4-6"))
                        catalogHasOpus46 = true;
                }
                catalogDone = true;
            });
        QTRY_VERIFY(catalogDone);
        QVERIFY(catalogHasClaude);
        QVERIFY(catalogHasOpus46);
        adapter.stop();
    }

    // A Claude tool approval shows what the tool will run, survives Codex stopping, and its
    // answer reaches the sidecar.
    void approvalShowsToolInputAndSurvivesCodexStop() {
        for (const QString decision : {QStringLiteral("accept"), QStringLiteral("decline")}) {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const QString claude = directory.filePath(QStringLiteral("fake-claude"));
            const QString logPath = directory.filePath(QStringLiteral("claude.log"));
            QVERIFY(writeFakeClaude(claude, logPath));
            LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
            QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
            CodexAdapter codex(QStringLiteral("/missing-codex-for-claude-test"));
            ClaudeAdapter adapter(claude);
            SessionHost host(&journal, &codex, &adapter);
            const QString sessionId = startClaude(host, directory.path(), "needs-approval");
            QVERIFY(!sessionId.isEmpty());
            const auto events = eventsOf(journal, sessionId);
            QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("approval.requested")).size(), 1, 3000);
            const QJsonObject card =
                events(QStringLiteral("approval.requested")).first()["payload"].toObject();
            QCOMPARE(card.value(QStringLiteral("kind")).toString(), QStringLiteral("approval"));
            QVERIFY(card.value(QStringLiteral("command")).toString().contains("ls -la"));

            // Codex failing to start stops the Codex provider; Claude's approval stays.
            QSignalSpy codexStopped(&codex, &CodexAdapter::stopped);
            host.handleProviderQuery(request(QStringLiteral("provider.models"), "models"),
                                     [](const QJsonObject&, const QString&) {});
            QTRY_VERIFY_WITH_TIMEOUT(codexStopped.count() > 0, 3000);

            const QJsonObject resolved = host.handleCommand(
                request(QStringLiteral("approval.resolve"), QStringLiteral("resolve-") + decision,
                        {{QStringLiteral("approval_id"), card.value("approval_id")},
                         {QStringLiteral("resolution"),
                          QJsonObject{{QStringLiteral("decision"), decision}}}},
                        sessionId, 1),
                QStringLiteral("desktop-test"));
            QVERIFY2(resolved.value(QStringLiteral("ok")).toBool(),
                     qPrintable(QJsonDocument(resolved).toJson(QJsonDocument::Compact)));
            const QByteArray expected = decision == QStringLiteral("accept")
                                            ? QByteArrayLiteral(R"("allow": true)")
                                            : QByteArrayLiteral(R"("allow": false)");
            QTRY_VERIFY_WITH_TIMEOUT(logText(logPath).contains(expected), 3000);
            QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.completed")).size(), 1, 3000);
            adapter.stop();
        }
    }

    // Interrupting before the SDK has named a thread still ends the turn.
    void interruptBeforeThreadEndsTurn() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString claude = directory.filePath(QStringLiteral("fake-claude"));
        const QString logPath = directory.filePath(QStringLiteral("claude.log"));
        QVERIFY(writeFakeClaude(claude, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-claude-test"));
        ClaudeAdapter adapter(claude);
        SessionHost host(&journal, &codex, &adapter);
        const QString sessionId = startClaude(host, directory.path(), "hang");
        QVERIFY(!sessionId.isEmpty());
        const auto events = eventsOf(journal, sessionId);
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.started")).size(), 1, 3000);
        const QJsonObject interrupted =
            host.handleCommand(request(QStringLiteral("session.interrupt"),
                                       QStringLiteral("interrupt"), {}, sessionId, 1),
                               QStringLiteral("desktop-test"));
        QVERIFY2(interrupted.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(interrupted).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.completed")).size(), 1, 3000);
        QCOMPARE(events(QStringLiteral("turn.completed")).first()["payload"]["status"].toString(),
                 QStringLiteral("interrupted"));
        const QJsonObject listed =
            host.handleCommand(request(QStringLiteral("session.list"), QStringLiteral("list")),
                               QStringLiteral("desktop-test"));
        QCOMPARE(listed["sessions"].toArray().first()["state"].toString(), QStringLiteral("ready"));
        adapter.stop();
    }

    // Steering reaches a running Claude turn, even before the SDK has named a thread, and
    // is journaled so the transcript keeps it.
    void steersRunningTurn() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString claude = directory.filePath(QStringLiteral("fake-claude"));
        const QString logPath = directory.filePath(QStringLiteral("claude.log"));
        QVERIFY(writeFakeClaude(claude, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-claude-test"));
        ClaudeAdapter adapter(claude);
        SessionHost host(&journal, &codex, &adapter);
        const QString sessionId = startClaude(host, directory.path(), "hang");
        QVERIFY(!sessionId.isEmpty());
        const auto events = eventsOf(journal, sessionId);
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.started")).size(), 1, 3000);
        const QJsonObject steered = host.handleCommand(
            request(QStringLiteral("session.steer"), QStringLiteral("steer"),
                    {{QStringLiteral("prompt"), QStringLiteral("use the other branch")}}, sessionId,
                    1),
            QStringLiteral("desktop-test"));
        QVERIFY2(steered.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(steered).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.steered")).size(), 1, 3000);
        QCOMPARE(events(QStringLiteral("turn.steered")).first()["payload"]["text"].toString(),
                 QStringLiteral("use the other branch"));
        QVERIFY(logText(logPath).contains(
            QStringLiteral("steer:%1:use the other branch").arg(sessionId).toUtf8()));
        // The provider reads the skills a client wrapped around the text; the journal keeps
        // only what the user typed.
        const QJsonObject wrapped = host.handleCommand(
            request(QStringLiteral("session.steer"), QStringLiteral("steer-with-skill"),
                    {{QStringLiteral("prompt"), QStringLiteral("SKILL BODY. Request: run it")},
                     {QStringLiteral("display_text"), QStringLiteral("run it")}},
                    sessionId, 1),
            QStringLiteral("desktop-test"));
        QVERIFY2(wrapped.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(wrapped).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.steered")).size(), 2, 3000);
        QCOMPARE(events(QStringLiteral("turn.steered")).at(1)["payload"]["text"].toString(),
                 QStringLiteral("run it"));
        QTRY_VERIFY(logText(logPath).contains(
            QStringLiteral("steer:%1:SKILL BODY. Request: run it").arg(sessionId).toUtf8()));
        QVERIFY(events(QStringLiteral("turn.steer_failed")).isEmpty());
        adapter.stop();
    }

    // A steer carries images to the running turn: a phone's as staged attachments, a local
    // client's also by path. turn.steered keeps media references so every client can draw them.
    void steerCarriesImagesToTheRunningTurn() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString claude = directory.filePath(QStringLiteral("fake-claude"));
        const QString logPath = directory.filePath(QStringLiteral("claude.log"));
        QVERIFY(writeFakeClaude(claude, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-claude-test"));
        ClaudeAdapter adapter(claude);
        SessionHost host(&journal, &codex, &adapter);
        const QString sessionId = startClaude(host, directory.path(), "hang");
        QVERIFY(!sessionId.isEmpty());
        const auto events = eventsOf(journal, sessionId);
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.started")).size(), 1, 3000);
        const auto sha = [](const QByteArray& bytes) {
            return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        };

        const QByteArray staged("\x89PNG\r\n\x1a\nstaged by the phone");
        QVERIFY(journal.beginAttachment(sessionId, 1, "phone-image", 0, "screen.png", "image/png",
                                        staged.size(), 1, QString::fromLatin1(sha(staged))));
        QVERIFY(journal.appendAttachmentChunk(sessionId, 1, "phone-image", 1, 0, staged,
                                              QString::fromLatin1(sha(staged))));
        QVERIFY(journal.commitAttachment(sessionId, 1, "phone-image", 1));
        const QString macFile = directory.filePath(QStringLiteral("private.png"));
        const QByteArray macBytes("\x89PNG\r\n\x1a\na file on the Mac");
        {
            QFile file(macFile);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(macBytes);
        }
        const QJsonArray macImage{
            QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"), QStringLiteral("look")}},
            QJsonObject{{QStringLiteral("type"), QStringLiteral("localImage")},
                        {QStringLiteral("path"), macFile}}};

        // The phone's path to a Mac file is dropped; its staged image goes through.
        auto answers = std::make_shared<QList<QJsonObject>>();
        host.handleCommandDeferred(
            request(QStringLiteral("session.steer"), QStringLiteral("phone-steer"),
                    {{QStringLiteral("prompt"), QStringLiteral("look")},
                     {QStringLiteral("input"), macImage},
                     {QStringLiteral("attachment_ids"), QJsonArray{QStringLiteral("phone-image")}}},
                    sessionId, 1),
            QStringLiteral("cloud-control-v2"),
            [answers](const QJsonObject& result) { answers->append(result); });
        QTRY_COMPARE_WITH_TIMEOUT(answers->size(), 1, 3000);
        QVERIFY2(answers->first().value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(answers->first()).toJson(QJsonDocument::Compact)));
        QCOMPARE(logText(logPath).count("steer-image:"), 1);
        QVERIFY(logText(logPath).contains("steer-image:" + sha(staged)));
        const QJsonArray phoneImages =
            events(QStringLiteral("turn.steered")).first()["payload"]["images"].toArray();
        QCOMPARE(phoneImages.size(), 1);
        QCOMPARE(phoneImages.first()["name"].toString(), QStringLiteral("screen.png"));
        QVERIFY(phoneImages.first()["media_ref"].toString().startsWith("sha256:"));

        // The desktop names its own files.
        const QJsonObject desktop = host.handleCommand(
            request(QStringLiteral("session.steer"), QStringLiteral("desktop-steer"),
                    {{QStringLiteral("prompt"), QStringLiteral("look")},
                     {QStringLiteral("input"), macImage}},
                    sessionId, 1),
            QStringLiteral("desktop-test"));
        QVERIFY2(desktop.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(desktop).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.steered")).size(), 2, 3000);
        QVERIFY(logText(logPath).contains("steer-image:" + sha(macBytes)));
        QCOMPARE(events(QStringLiteral("turn.steered")).at(1)["payload"]["images"].toArray().size(),
                 1);

        adapter.stop();
    }

    // A phone never saw the Mac's files: a path it names in a new message's input is dropped
    // rather than handed to the agent and copied into the media store the phone reads.
    void phoneCannotNameMacFiles() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString claude = directory.filePath(QStringLiteral("fake-claude"));
        const QString logPath = directory.filePath(QStringLiteral("claude.log"));
        QVERIFY(writeFakeClaude(claude, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-claude-test"));
        ClaudeAdapter adapter(claude);
        SessionHost host(&journal, &codex, &adapter);
        const QString macFile = directory.filePath(QStringLiteral("private.png"));
        {
            QFile file(macFile);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("\x89PNG\r\n\x1a\na file on the Mac");
        }
        const QJsonArray macImage{
            QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"), QStringLiteral("look")}},
            QJsonObject{{QStringLiteral("type"), QStringLiteral("localImage")},
                        {QStringLiteral("path"), macFile}}};
        const QJsonObject phoneStart = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("phone-start"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"), QStringLiteral("look")},
                     {QStringLiteral("input"), macImage},
                     {QStringLiteral("options"),
                      QJsonObject{{QStringLiteral("provider"), QStringLiteral("claude")},
                                  {QStringLiteral("model"), QStringLiteral("sonnet")}}}}),
            QStringLiteral("cloud-control-v2"));
        QVERIFY2(phoneStart.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(phoneStart).toJson(QJsonDocument::Compact)));
        const auto phoneEvents =
            eventsOf(journal, phoneStart.value(QStringLiteral("session_id")).toString());
        QTRY_COMPARE_WITH_TIMEOUT(phoneEvents(QStringLiteral("user.message")).size(), 1, 3000);
        QVERIFY(phoneEvents(QStringLiteral("user.message"))
                    .first()["payload"]["images"]
                    .toArray()
                    .isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(logText(logPath).count("policy:"), 1, 3000);
        QVERIFY(!logText(logPath).contains(macFile.toUtf8()));
        adapter.stop();
    }

    // A phone's steer is answered only once the provider has taken or refused it, and names
    // the turn it was meant for; a redelivery answers from the journal without steering again.
    void deferredSteerAnswersWithTheProviderOutcome() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString claude = directory.filePath(QStringLiteral("fake-claude"));
        const QString logPath = directory.filePath(QStringLiteral("claude.log"));
        QVERIFY(writeFakeClaude(claude, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-claude-test"));
        ClaudeAdapter adapter(claude);
        SessionHost host(&journal, &codex, &adapter);
        const QString sessionId = startClaude(host, directory.path(), "hang");
        QVERIFY(!sessionId.isEmpty());
        const auto events = eventsOf(journal, sessionId);
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.started")).size(), 1, 3000);

        const auto steer = [&](const QString& id, const QString& prompt) {
            auto answers = std::make_shared<QList<QJsonObject>>();
            host.handleCommandDeferred(
                request(QStringLiteral("session.steer"), id, {{QStringLiteral("prompt"), prompt}},
                        sessionId, 1),
                QStringLiteral("cloud-control-v2"),
                [answers](const QJsonObject& result) { answers->append(result); });
            return answers;
        };

        const auto taken = steer(QStringLiteral("steer-taken"), QStringLiteral("keep going"));
        QTRY_COMPARE_WITH_TIMEOUT(taken->size(), 1, 3000);
        const QJsonObject result = taken->first();
        QVERIFY2(result.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(result).toJson(QJsonDocument::Compact)));
        QVERIFY(result.value(QStringLiteral("applied")).toBool());
        QCOMPARE(result.value(QStringLiteral("command_id")).toString(),
                 QStringLiteral("steer-taken"));
        QCOMPARE(result.value(QStringLiteral("turn_sequence")).toInteger(), 1);
        QCOMPARE(events(QStringLiteral("turn.steered")).size(), 1);
        QCOMPARE(events(QStringLiteral("turn.steered")).first()["payload"]["command_id"].toString(),
                 QStringLiteral("steer-taken"));
        const auto again = steer(QStringLiteral("steer-taken"), QStringLiteral("keep going"));
        QCOMPARE(again->size(), 1);
        QCOMPARE(again->first(), result);
        QCOMPARE(logText(logPath).count("steer:"), 1);

        const auto refused =
            steer(QStringLiteral("steer-refused"), QStringLiteral("refused steer"));
        QTRY_COMPARE_WITH_TIMEOUT(refused->size(), 1, 3000);
        QVERIFY(!refused->first().value(QStringLiteral("ok")).toBool());
        QCOMPARE(refused->first().value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("steer_rejected"));
        QCOMPARE(events(QStringLiteral("turn.steer_failed")).size(), 1);

        // The provider has the text but has not answered: a redelivery must not steer twice,
        // and cannot claim the text was taken.
        const auto pending =
            steer(QStringLiteral("steer-unanswered"), QStringLiteral("unanswered steer"));
        QTRY_VERIFY_WITH_TIMEOUT(logText(logPath).contains("unanswered steer"), 3000);
        QVERIFY(pending->isEmpty());
        const auto redelivered =
            steer(QStringLiteral("steer-unanswered"), QStringLiteral("unanswered steer"));
        QCOMPARE(redelivered->size(), 1);
        QCOMPARE(redelivered->first().value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("steer_outcome_unknown"));
        QCOMPARE(logText(logPath).count("unanswered steer"), 1);
        adapter.stop();
    }

    // A conversation moves Codex -> Claude -> Codex between turns. Each provider keeps its own
    // thread, and the one taking over is pointed at a transcript of the turns so far.
    void switchesProvidersThroughHandoffTranscript_data() {
        QTest::addColumn<bool>("acknowledged");
        QTest::addColumn<bool>("restart");
        QTest::newRow("before-desktop-ack") << false << false;
        QTest::newRow("after-desktop-ack") << true << false;
        QTest::newRow("after-daemon-restart") << true << true;
    }

    void switchesProvidersThroughHandoffTranscript() {
        QFETCH(bool, acknowledged);
        QFETCH(bool, restart);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString claude = directory.filePath(QStringLiteral("fake-claude"));
        const QString claudeLog = directory.filePath(QStringLiteral("claude.log"));
        QVERIFY(writeFakeClaude(claude, claudeLog));
        const QString codexPath = directory.filePath(QStringLiteral("fake-codex"));
        const QString codexLog = directory.filePath(QStringLiteral("codex.log"));
        QVERIFY(writeFakeCodex(codexPath, codexLog));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(codexPath);
        ClaudeAdapter adapter(claude);
        auto host = std::make_unique<SessionHost>(&journal, &codex, &adapter);
        const auto turn = [&](const QString& id, const QString& provider, const QString& prompt) {
            QJsonObject message = request(
                QStringLiteral("session.start_turn"), QStringLiteral("turn-") + provider + prompt,
                {{QStringLiteral("worktree_path"), directory.path()},
                 {QStringLiteral("prompt"), prompt},
                 {QStringLiteral("options"),
                  QJsonObject{{QStringLiteral("provider"), provider},
                              {QStringLiteral("permission"), QStringLiteral("workspace-write")}}}},
                id, id.isEmpty() ? -1 : 1);
            return host->handleCommand(message, QStringLiteral("desktop-test"));
        };

        const QJsonObject first =
            turn({}, QStringLiteral("codex"), QStringLiteral("First question"));
        QVERIFY2(first.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(first).toJson(QJsonDocument::Compact)));
        const QString sessionId = first.value(QStringLiteral("session_id")).toString();
        const auto events = [&](const QString& type) {
            QJsonArray matches;
            for (const QJsonValue& value :
                 journal.compactedEventsAfter(sessionId, 1, 0, 500).events) {
                if (value.toObject().value("event_type").toString() == type)
                    matches.append(value);
            }
            return matches;
        };
        const auto acknowledge = [&] {
            if (!acknowledged)
                return;
            const QJsonArray retained = journal.compactedEventsAfter(sessionId, 1, 0, 500).events;
            QVERIFY(!retained.isEmpty());
            QVERIFY(
                journal.acknowledgeCursor("desktop-test", sessionId, 1,
                                          retained.last().toObject().value("cursor").toInteger()));
            QVERIFY(journal.eventsAfter(sessionId, 1, 0).isEmpty());
        };
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.completed")).size(), 1, 5000);

        acknowledge();
        const QJsonObject toClaude =
            turn(sessionId, QStringLiteral("claude"), QStringLiteral("Carry on"));
        QVERIFY2(toClaude.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(toClaude).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.completed")).size(), 2, 5000);
        QCOMPARE(events(QStringLiteral("session.provider_changed")).size(), 1);
        const QString transcript =
            QDir(directory.path())
                .filePath(QStringLiteral("handoff/%1/transcript.md").arg(sessionId));
        QVERIFY(QFileInfo::exists(transcript));
        const QByteArray written = logText(transcript);
        QVERIFY(written.contains("First question"));
        QVERIFY(written.contains("Codex"));
        QVERIFY(!written.contains("Carry on"));
        const QByteArray claudeSeen = logText(claudeLog);
        QVERIFY(claudeSeen.contains("[KetPlus handoff]"));
        QVERIFY(claudeSeen.contains(transcript.toUtf8()));
        QVERIFY(claudeSeen.contains("Carry on"));
        QVERIFY(events(QStringLiteral("item.completed")).last()["payload"]["provider"].toString() ==
                QStringLiteral("claude"));

        acknowledge();
        if (restart) {
            host.reset();
            host = std::make_unique<SessionHost>(&journal, &codex, &adapter);
        }
        const QJsonObject back =
            turn(sessionId, QStringLiteral("codex"), QStringLiteral("Back again"));
        QVERIFY2(back.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(back).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.completed")).size(), 3, 5000);
        QCOMPARE(events(QStringLiteral("session.provider_changed")).size(), 2);
        // Returning to A uses a fresh native thread, carrying only the latest handoff.
        QCOMPARE(logText(codexLog).count("turn:thread-1"), 1);
        QCOMPARE(logText(codexLog).count("turn:thread-2"), 1);
        QJsonObject returnedContext;
        codex.call("thread/read", {{"threadId", journal.sessions().first().threadId}},
                   [&](const QJsonObject& result, const QString&) { returnedContext = result; });
        QTRY_VERIFY(returnedContext.value("thread").toObject().value("preview").toString().contains(
            "You have not seen any of it."));
        const auto references = provider_handoff::threadReferences(
            journal.sessions().first().adapter, journal.sessions().first().threadId,
            QJsonDocument::fromJson(journal.sessions().first().providerThreads.toUtf8()).object());
        QVERIFY(references.contains(QJsonObject{{"provider", "codex"}, {"thread_id", "thread-1"}}));
        QVERIFY(references.contains(QJsonObject{{"provider", "codex"}, {"thread_id", "thread-2"}}));
        QVERIFY(logText(transcript).contains("Carry on"));
        QVERIFY(logText(transcript).contains("**Claude:**\nfinished"));
        QVERIFY(logText(transcript).contains("First question"));

        // A Claude run kept open for background tasks finishes after the move back to Codex.
        // It must not replace Codex's thread nor end Codex's state.
        const auto codexThread = journal.sessions().first().threadId;
        emit adapter.canonicalEvent(sessionId, QStringLiteral("turn.completed"),
                                    {{QStringLiteral("thread_id"), QStringLiteral("late-claude")},
                                     {QStringLiteral("status"), QStringLiteral("completed")}});
        QCOMPARE(journal.sessions().first().threadId, codexThread);
        QCOMPARE(journal.sessions().first().adapter, QStringLiteral("codex"));
        QVERIFY(journal.sessions().first().providerThreads.contains("late-claude"));
        QCOMPARE(events(QStringLiteral("turn.completed")).size(), 3);

        const QJsonObject deleted = host->handleCommand(
            request(QStringLiteral("session.delete"), QStringLiteral("delete"), {}, sessionId, 1),
            QStringLiteral("desktop-test"));
        QVERIFY2(deleted.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(deleted).toJson(QJsonDocument::Compact)));
        QVERIFY(!QFileInfo::exists(transcript));
        adapter.stop();
        codex.stop();
    }

    // The transcript keeps what was said verbatim and tool activity short.
    void healsAThreadAnOlderDaemonTookFromThePreviousProvider() {
        // Older daemons let a lingering Claude run write its thread over Codex's. Resuming it
        // failed every turn ("thread not found"); a restart now starts a fresh Codex thread
        // pointed at the handoff transcript.
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString codexPath = directory.filePath(QStringLiteral("fake-codex"));
        const QString codexLog = directory.filePath(QStringLiteral("codex.log"));
        QVERIFY(writeFakeCodex(codexPath, codexLog));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        StoredSession broken;
        broken.id = QStringLiteral("broken");
        broken.worktreePath = QFileInfo(directory.path()).canonicalFilePath();
        broken.adapter = QStringLiteral("codex");
        broken.state = QStringLiteral("failed");
        broken.generation = 1;
        broken.threadId = QStringLiteral("claude-thread");
        broken.providerThreads = QStringLiteral(R"({"claude":{"thread_id":"claude-thread"}})");
        QVERIFY(journal.upsertSession(broken));
        CodexAdapter codex(codexPath);
        SessionHost host(&journal, &codex);
        QVERIFY(journal.sessions().first().threadId.isEmpty());
        const QJsonObject healed = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("healed"),
                    {{QStringLiteral("prompt"), QStringLiteral("After restart")}},
                    QStringLiteral("broken"), 1),
            QStringLiteral("desktop-test"));
        QVERIFY2(healed.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(healed).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE_WITH_TIMEOUT(journal.sessions().first().state, QStringLiteral("ready"), 5000);
        QVERIFY(!journal.sessions().first().threadId.isEmpty());
        QVERIFY(journal.sessions().first().threadId != QStringLiteral("claude-thread"));
        QVERIFY(!logText(codexLog).contains("claude-thread"));
        codex.stop();
    }

    void transcriptSummarizesToolActivity() {
        namespace handoff = ketplus::daemon::provider_handoff;
        const auto event = [](qint64 cursor, qint64 turn, const char* type, QJsonObject payload) {
            return QJsonObject{{"cursor", cursor},
                               {"turn_sequence", turn},
                               {"event_type", QString::fromLatin1(type)},
                               {"payload", payload}};
        };
        const QString output = QString(2000, QLatin1Char('x'));
        const QString text = handoff::renderTranscript(QJsonArray{
            event(1, 0, "session.created", {{"adapter", "codex"}}),
            event(2, 1, "user.message", {{"text", "Fix the build"}}),
            event(3, 1, "item.completed",
                  {{"item", QJsonObject{{"type", "commandExecution"},
                                        {"command", "cmake --build build"},
                                        {"exitCode", 1},
                                        {"aggregatedOutput", output}}}}),
            event(4, 1, "item.completed",
                  {{"item",
                    QJsonObject{{"type", "fileChange"},
                                {"changes", QJsonArray{QJsonObject{{"path", "src/a.cpp"}}}}}}}),
            event(5, 1, "item.completed",
                  {{"item", QJsonObject{{"type", "agentMessage"}, {"text", "Fixed a.cpp."}}}}),
            event(6, 1, "session.provider_changed", {{"from", "codex"}, {"to", "claude"}}),
            event(7, 2, "user.message", {{"text", "Now add a test"}}),
        });
        QVERIFY(text.contains("## Turn 1 — Codex [cursor 2]"));
        QVERIFY(text.contains("**User:**\nFix the build"));
        QVERIFY(text.contains("- Ran `cmake --build build` (exit 1)"));
        QVERIFY(text.contains("more characters]"));
        QVERIFY(!text.contains(output));
        QVERIFY(text.contains("- Changed src/a.cpp"));
        QVERIFY(text.contains("**Codex:**\nFixed a.cpp."));
        QVERIFY(text.contains("Switched from Codex to Claude."));
        QVERIFY(text.contains("## Turn 2 — Claude [cursor 7]"));
        QVERIFY(handoff::handoffNote("codex", "/t.md", 0).contains("You have not seen any of it."));
        QVERIFY(handoff::handoffNote("claude", "/t.md", 42).contains("greater than 42"));
    }

    void reportsAndControlsBackgroundTasksAcrossRestarts() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString claude = directory.filePath(QStringLiteral("fake-claude"));
        const QString logPath = directory.filePath(QStringLiteral("claude.log"));
        QVERIFY(writeFakeClaude(claude, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-claude-test"));
        const auto listed = [](SessionHost& host) {
            return host
                .handleCommand(request(QStringLiteral("session.list"), QStringLiteral("list")),
                               QStringLiteral("desktop-test"))["sessions"][0]["background_tasks"]
                .toArray();
        };
        const auto first = [&listed](SessionHost& host) { return listed(host).first().toObject(); };
        const auto command = [](SessionHost& host, const QString& type, const QString& sessionId,
                                const QString& taskId) {
            auto answers = std::make_shared<QList<QJsonObject>>();
            host.handleCommandDeferred(
                request(type, type + taskId, {{QStringLiteral("task_id"), taskId}}, sessionId, 1),
                QStringLiteral("cloud-control-v2"),
                [answers](const QJsonObject& result) { answers->append(result); });
            if (!QTest::qWaitFor([&answers] { return !answers->isEmpty(); }, 3000))
                return QJsonObject{};
            return answers->first();
        };
        QString sessionId;
        {
            ClaudeAdapter adapter(claude);
            SessionHost host(&journal, &codex, &adapter);
            sessionId = startClaude(host, directory.path(), QStringLiteral("background"));
            const auto events = eventsOf(journal, sessionId);
            QTRY_VERIFY_WITH_TIMEOUT(!events(QStringLiteral("turn.completed")).isEmpty(), 3000);
            // A row the daemon cannot describe is dropped rather than shown.
            QCOMPARE(events(QStringLiteral("tasks.changed")).size(), 1);
            QCOMPARE(listed(host).size(), 1);
            QCOMPARE(first(host)["status"].toString(), QStringLiteral("running"));
            QCOMPARE(first(host)["command"].toString(), QStringLiteral("sleep 100"));

            const QJsonObject output = command(host, QStringLiteral("session.task.output"),
                                               sessionId, QStringLiteral("t1"));
            QVERIFY(output["ok"].toBool());
            // The phone gets the last 8 KiB, whatever the provider sent.
            QCOMPARE(output["output"].toString().size(), 8 * 1024);
            QVERIFY(output["truncated"].toBool());
            QCOMPARE(output["total_bytes"].toInteger(), 15000);
            QCOMPARE(command(host, QStringLiteral("session.task.stop"), sessionId,
                             QStringLiteral("unlisted"))["error_code"]
                         .toString(),
                     QStringLiteral("task_not_found"));
            QVERIFY(!logText(logPath).contains("task-stop:"));
        }
        // The provider that ran it is gone, so a restarted daemon reports it ended.
        ClaudeAdapter adapter(claude);
        SessionHost host(&journal, &codex, &adapter);
        QCOMPARE(listed(host).size(), 1);
        QCOMPARE(first(host)["status"].toString(), QStringLiteral("stopped"));
        QCOMPARE(first(host)["reason"].toString(), QStringLiteral("worker_restart"));
        QCOMPARE(command(host, QStringLiteral("session.task.stop"), sessionId,
                         QStringLiteral("t1"))["error_code"]
                     .toString(),
                 QStringLiteral("task_not_running"));

        const QJsonObject resumed = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("again"),
                    {{QStringLiteral("prompt"), QStringLiteral("background")}}, sessionId, 1),
            QStringLiteral("desktop-test"));
        QVERIFY2(resumed["ok"].toBool(), qPrintable(QJsonDocument(resumed).toJson()));
        QTRY_COMPARE_WITH_TIMEOUT(first(host)["status"].toString(), QStringLiteral("running"),
                                  3000);
        QVERIFY(command(host, QStringLiteral("session.task.stop"), sessionId,
                        QStringLiteral("t1"))["ok"]
                    .toBool());
        QVERIFY(logText(logPath).contains("task-stop:t1"));
        QTRY_COMPARE_WITH_TIMEOUT(first(host)["status"].toString(), QStringLiteral("stopped"),
                                  3000);
        QVERIFY(!first(host).contains(QStringLiteral("reason")));
    }

    void rejectsClaudeWhenSidecarIsMissing() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-claude-test"));
        SessionHost host(&journal, &codex);
        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("missing-claude"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"), QStringLiteral("Hello")},
                     {QStringLiteral("options"),
                      QJsonObject{{QStringLiteral("provider"), QStringLiteral("claude")}}}}),
            QStringLiteral("desktop-test"));
        QVERIFY(!started.value(QStringLiteral("ok")).toBool());
        QCOMPARE(started.value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("provider_unavailable"));
    }
};

QTEST_GUILESS_MAIN(ClaudeAdapterTest)
#include "ClaudeAdapterTest.moc"
