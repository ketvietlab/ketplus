#include "FakeCodex.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/ResponseGuidance.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

using namespace ketplus::daemon;

namespace {
bool git(const QString& root, const QStringList& args) {
    QProcess process;
    process.start("git", QStringList{"-C", root} + args);
    return process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0;
}

QJsonObject request(const QString& type, const QString& id, const QJsonObject& payload = {}) {
    return {{"protocol_version", 0},   {"message_type", type},
            {"message_id", id},        {"session_id", "conversation"},
            {"session_generation", 1}, {"sent_at", protocol::timestamp()},
            {"payload", payload}};
}

struct Fixture {
    QTemporaryDir directory;
    QString a{directory.filePath("repo")};
    QString b{directory.filePath("feature")};
    QString log{directory.filePath("provider.log")};
    std::unique_ptr<LocalJournal> journal;
    std::unique_ptr<CodexAdapter> provider;
    std::unique_ptr<SessionHost> host;

    bool initialize() {
        if (!directory.isValid() || !QDir().mkpath(a) || !git(a, {"init", "-b", "main"}) ||
            !git(a, {"-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "-c",
                     "commit.gpgsign=false", "commit", "--allow-empty", "-m", "fixture"}) ||
            !git(a, {"worktree", "add", "-b", "feature", b}))
            return false;
        a = QFileInfo(a).canonicalFilePath();
        b = QFileInfo(b).canonicalFilePath();
        const auto executable = directory.filePath("fake-codex");
        if (!writeFakeCodex(executable, log))
            return false;
        journal = std::make_unique<LocalJournal>(directory.filePath("journal.sqlite"));
        StoredSession session;
        session.id = "conversation";
        session.worktreePath = a;
        session.adapter = "codex";
        session.state = "ready";
        if (!journal->isOpen() || !journal->upsertSession(session))
            return false;
        provider = std::make_unique<CodexAdapter>(executable);
        host = std::make_unique<SessionHost>(journal.get(), provider.get());
        return true;
    }
    QJsonObject call(const QString& type, const QString& id, QJsonObject payload = {}) {
        return host->handleCommand(request(type, id, payload), "isolated-context-test");
    }
    QJsonObject move(const QString& path, int revision, const QString& id) {
        return call("session.worktree.switch", id,
                    {{"worktree_path", path}, {"expected_context_revision", revision}});
    }
    QByteArray output() const {
        QFile file(log);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    }
};
} // namespace

class DaemonWorktreeContextTest final : public QObject {
    Q_OBJECT
  private slots:
    void followsExplicitWorkingCheckoutAfterTurn_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("follows");
        for (const QString& kind :
             {"command", "file", "subdirectory", "failed", "relative", "foreign", "prose"})
            QTest::newRow(qPrintable(kind))
                << kind << (kind == "command" || kind == "file" || kind == "subdirectory");
    }
    void followsExplicitWorkingCheckoutAfterTurn() {
        QFETCH(QString, kind);
        QFETCH(bool, follows);
        Fixture f;
        QVERIFY(f.initialize());
        QVERIFY(f.call("session.start_turn", "start", {{"prompt", "hang"}}).value("ok").toBool());
        QTRY_COMPARE(f.journal->sessions().first().state, QString("running"));
        const auto active = f.journal->sessions().first();
        QString cwd = f.b;
        if (kind == "relative")
            cwd = "../feature";
        if (kind == "foreign") {
            cwd = f.directory.filePath("foreign");
            QVERIFY(QDir().mkpath(cwd));
        }
        if (kind == "subdirectory") {
            cwd += "/src";
            QVERIFY(QDir().mkpath(cwd));
        }
        QJsonObject item{{"id", "working-checkout"},
                         {"type", "commandExecution"},
                         {"status", kind == "failed" ? "failed" : "completed"},
                         {"cwd", cwd},
                         {"exitCode", kind == "failed" ? 1 : 0}};
        if (kind == "file") {
            QFile file(f.b + "/result.txt");
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.close();
            item = {{"id", "changed"},
                    {"type", "fileChange"},
                    {"status", "completed"},
                    {"changes", QJsonArray{QJsonObject{{"path", file.fileName()}}}}};
        }
        if (kind == "prose")
            item = {{"id", "answer"}, {"type", "agentMessage"}, {"text", f.b}};
        f.provider->notificationReceived(
            "item/completed",
            {{"threadId", active.threadId}, {"turnId", active.turnId}, {"item", item}});
        QCOMPARE(f.journal->sessions().first().worktreePath, f.a);
        f.provider->notificationReceived(
            "turn/completed",
            {{"threadId", active.threadId},
             {"turn", QJsonObject{{"id", active.turnId}, {"status", "completed"}}}});
        QCOMPARE(f.journal->sessions().first().worktreePath, follows ? f.b : f.a);
        QCOMPARE(f.journal->sessions().first().contextRevision, follows ? 2 : 1);
        QCOMPARE(f.journal->turnContextRevision("conversation", 1, active.turnId), 1);
    }

    // The guidance travels once in the thread's developer instructions. Repeating it in every
    // user message would add another copy to the provider history on each turn.
    void responseGuidanceIsProviderOnly() {
        Fixture f;
        QVERIFY(f.initialize());
        QFile executable(f.directory.filePath("fake-codex"));
        QVERIFY(executable.open(QIODevice::ReadOnly));
        auto source = executable.readAll();
        executable.close();
        source.replace("        text = message['params']['input'][0]['text']",
                       "        text = message['params']['input'][0]['text']\n"
                       "        log('provider-input:' + json.dumps(message['params']['input']))");
        source.replace(
            "    elif method == 'turn/steer' or method == 'turn/interrupt':",
            "    elif method == 'turn/steer' or method == 'turn/interrupt':\n"
            "        log('provider-input:' + json.dumps(message['params'].get('input', [])))");
        QVERIFY(executable.open(QIODevice::WriteOnly | QIODevice::Truncate));
        executable.write(source);
        executable.close();
        QVERIFY(
            f.call("session.start_turn", "start-guidance", {{"prompt", "hang"}})["ok"].toBool());
        QTRY_COMPARE(f.journal->sessions().first().state, QString("running"));
        QVERIFY(f.call("session.steer", "steer-guidance", {{"prompt", "continue"}})["ok"].toBool());
        QTRY_COMPARE(f.output().count("provider-input:"), 2);
        int setups = 0;
        for (const auto& line : f.output().split('\n')) {
            if (line.startsWith("provider-input:")) {
                const auto input = QJsonDocument::fromJson(line.mid(15)).array();
                QCOMPARE(input.size(), 1);
                QVERIFY(!input.first()["text"].toString().contains("ketplus_response_preferences"));
                continue;
            }
            if (!line.startsWith("setup:"))
                continue;
            ++setups;
            const auto suffix =
                QJsonDocument::fromJson(line.mid(6)).object()["developerInstructions"].toString();
            QCOMPARE(suffix.count("<ketplus_response_preferences>"), 1);
            QVERIFY(suffix.endsWith(ketplus::daemon::responseGuidance()));
            QVERIFY(suffix.contains("refer to yourself as 'tui'"));
            QVERIFY(suffix.contains("next task or action and who will do it"));
            QVERIFY(suffix.contains("no forced flirting"));
            QVERIFY(suffix.contains("Before ending every turn"));
            QVERIFY(suffix.contains(
                "Task | Status | Result/evidence or remaining work | Next action and owner"));
            QVERIFY(suffix.contains("including unfinished work from earlier turns"));
            QVERIFY(suffix.contains("not a reason to stop authorized work early"));
            QVERIFY(suffix.contains("after you merge a pull request, remove the linked worktree"));
            QVERIFY(suffix.contains("Never remove or clean the repository's main checkout"));
            QVERIFY(suffix.contains("make removal the last command of the turn"));
            QVERIFY(suffix.contains(
                "`git -C <main checkout> worktree remove <path>` without `--force`"));
            QVERIFY(
                suffix.contains("automatically resume the unfinished work from before the steer"));
            QVERIFY(suffix.contains("explicitly cancels, pauses or replaces it"));
            QVERIFY(suffix.contains("After context compaction"));
            QVERIFY(suffix.contains("absolute worktree path and current branch"));
            QVERIFY(suffix.contains("do not silently fall back to another checkout"));
            QVERIFY(!suffix.contains("flirt-comedy"));
        }
        QVERIFY(setups > 0);
        const auto events =
            QJsonDocument(f.journal->eventsAfter("conversation", 1, 0, 100)).toJson();
        QVERIFY(!events.contains("ketplus_response_preferences"));
        QVERIFY(!events.contains("flirt-comedy"));
        f.provider->stop();
    }

    void asyncAnswerAfterContextSwitch_data() {
        QTest::addColumn<bool>("completed");
        QTest::newRow("steer-running") << false;
        QTest::newRow("expired-after-completion") << true;
    }

    void asyncAnswerAfterContextSwitch() {
        QFETCH(bool, completed);
        Fixture f;
        QVERIFY(f.initialize());
        QVERIFY(f.move(f.b, 1, "move-before-question")["ok"].toBool());
        QFile executable(f.directory.filePath("fake-codex"));
        QVERIFY(executable.open(QIODevice::ReadOnly));
        QByteArray source = executable.readAll();
        executable.close();
        source.replace(
            "        if text in ('approve', 'approve-started'):",
            "        if text == 'async-fixture':\n"
            "            emit({'method': 'item/completed', 'params': {'threadId': thread_id, "
            "'turnId': turn_id, 'item': {'id': 'async-q', 'type': 'agentMessage', "
            "'delivery': 'async', 'questions': [{'title': 'Which scope?'}]}}})\n" +
                QByteArray(completed ? "            finish(thread_id, turn_id)\n" : "") +
                "            continue\n"
                "        if text in ('approve', 'approve-started'):");
        source.replace("        text = message['params']['input'][0]['text']",
                       "        text = message['params']['input'][0]['text']\n"
                       "        log('input:' + text)");
        source.replace(
            "    elif method == 'turn/steer' or method == 'turn/interrupt':",
            "    elif method == 'turn/steer' or method == 'turn/interrupt':\n"
            "        log('input:' + message['params'].get('input', [{}])[0].get('text', ''))");
        QVERIFY(executable.open(QIODevice::WriteOnly | QIODevice::Truncate));
        executable.write(source);
        executable.close();
        QVERIFY(f.call("session.start_turn", "ask-async",
                       {{"prompt", "async-fixture"}, {"expected_context_revision", 2}})["ok"]
                    .toBool());
        QJsonObject questionData;
        const auto findQuestion = [&] {
            for (const auto& value : f.journal->eventsAfter("conversation", 1, 0, 100)) {
                const auto event = value.toObject();
                if (event["event_type"] == "approval.requested") {
                    questionData = event["payload"].toObject();
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(findQuestion(), 3000);
        if (completed)
            QTRY_VERIFY_WITH_TIMEOUT(f.journal->sessions().first().turnId.isEmpty(), 3000);
        const auto question = questionData["questions"].toArray().first().toObject();
        const auto requestId = questionData["request_id"];
        const QJsonObject resolution{
            {"request_id", requestId},
            {"revision", questionData["revision"]},
            {"action", "answer"},
            {"answers", QJsonArray{QJsonObject{{"question_id", question["id"]},
                                               {"question_revision", question["revision"]},
                                               {"option_ids", QJsonArray{}},
                                               {"custom_answer", "keep latest twelve"}}}}};
        const auto result = f.call("question.answer", "answer-async",
                                   {{"request_id", requestId}, {"resolution", resolution}});
        if (completed) {
            QVERIFY(!result["ok"].toBool());
            QCOMPARE(result["error_code"].toString(), QStringLiteral("already_answered"));
            QCoreApplication::processEvents();
            QVERIFY(!f.output().contains("keep latest twelve"));
            QVERIFY(f.journal->sessions().first().turnId.isEmpty());
            QCOMPARE(f.journal->sessions().first().currentTurnSequence, 1);
        } else {
            QVERIFY2(result["ok"].toBool(), QJsonDocument(result).toJson().constData());
            QTRY_VERIFY_WITH_TIMEOUT(f.output().contains("keep latest twelve"), 3000);
        }
    }

    void steeringSendsImageBytesForDesktopAndPhone() {
        Fixture f;
        QVERIFY(f.initialize());
        // Keep a fake turn open and capture the exact provider request, without quota.
        QFile executable(f.directory.filePath("fake-codex"));
        QVERIFY(executable.open(QIODevice::ReadOnly));
        QByteArray source = executable.readAll();
        executable.close();
        source.replace("elif method == 'turn/steer' or method == 'turn/interrupt':",
                       "elif method == 'turn/steer' or method == 'turn/interrupt':\n"
                       "        log('steer-input:' + json.dumps(message['params']))\n"
                       "        emit({'method': 'item/started', 'params': {'threadId': "
                       "message['params']['threadId'], 'item': {'id': 'echo', 'type': "
                       "'userMessage', 'content': message['params']['input']}}})");
        QVERIFY(executable.open(QIODevice::WriteOnly | QIODevice::Truncate));
        executable.write(source);
        executable.close();
        QVERIFY(f.call("session.start_turn", "start-images", {{"prompt", "ask"}})["ok"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!f.journal->sessions().first().turnId.isEmpty(), 3000);
        const QByteArray bytes("\x89PNG\r\n\x1a\nsteering image");
        const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        QVERIFY(f.journal->beginAttachment("conversation", 1, "photo", 0, "photo.png", "image/png",
                                           bytes.size(), 1, digest));
        QVERIFY(f.journal->appendAttachmentChunk("conversation", 1, "photo", 1, 0, bytes, digest));
        QVERIFY(f.journal->commitAttachment("conversation", 1, "photo", 1));
        QJsonObject phoneResult;
        f.host->handleCommandDeferred(
            request("session.steer", "phone-steer",
                    {{"prompt", "look"}, {"attachment_ids", QJsonArray{"photo"}}}),
            "cloud-control-v2", [&](const QJsonObject& result) { phoneResult = result; });
        QTRY_VERIFY_WITH_TIMEOUT(!phoneResult.isEmpty(), 3000);
        QVERIFY(phoneResult["ok"].toBool());
        const QString path = f.directory.filePath("desktop.png");
        QFile image(path);
        QVERIFY(image.open(QIODevice::WriteOnly));
        image.write(bytes);
        image.close();
        QVERIFY(f.call("session.steer", "desktop-steer",
                       {{"prompt", ""},
                        {"input",
                         QJsonArray{QJsonObject{{"type", "text"}, {"text", ""}},
                                    QJsonObject{{"type", "localImage"}, {"path", path}}}}})["ok"]
                    .toBool());
        QVERIFY(QFile::remove(path));
        QTRY_COMPARE_WITH_TIMEOUT(f.output().count("steer-input:"), 2, 3000);
        for (const auto& event : f.journal->eventsAfter("conversation", 1, 0, 100)) {
            QVERIFY(event.toObject()["payload"].toObject()["item"].toObject()["type"] !=
                    "userMessage");
            if (event.toObject()["event_type"] == "user.message")
                QCOMPARE(event.toObject()["payload"].toObject()["command_id"].toString(),
                         QString("start-images"));
        }
        for (const QByteArray& line : f.output().split('\n')) {
            if (!line.startsWith("steer-input:"))
                continue;
            const QJsonArray input =
                QJsonDocument::fromJson(line.mid(12)).object()["input"].toArray();
            QCOMPARE(input.size(), 2);
            QCOMPARE(input.at(1)["type"].toString(), QString("image"));
            const QString url = input.at(1)["url"].toString();
            QCOMPARE(QByteArray::fromBase64(url.section(',', 1).toLatin1()), bytes);
        }
        f.provider->stop();
    }

    void resourcesStayOnRecordedSourceAcrossRoundTrip() {
        Fixture f;
        QVERIFY(f.initialize());
        QVERIFY(f.journal->recordSessionContext(f.journal->sessions().first()));
        const auto cursor = f.journal->appendEvent(
            "conversation", 1, "item.completed",
            {{"resource_refs",
              QJsonArray{QJsonObject{{"context_revision", 1}, {"path", "source.swift"}}}}});
        QVERIFY(cursor > 0);
        QVERIFY(f.move(f.b, 1, "move-b").value("ok").toBool());
        const auto resolved = f.call("session.resource.resolve", "resolve-b",
                                     {{"event_cursor", cursor}, {"path", "source.swift"}});
        QVERIFY(resolved.value("ok").toBool());
        QCOMPARE(resolved.value("worktree_path").toString(), f.a);
        QCOMPARE(resolved.value("resource_context_revision").toInteger(), 1);
        QVERIFY(f.move(f.a, 2, "move-a").value("ok").toBool());
        QCOMPARE(f.call("session.resource.resolve", "resolve-a",
                        {{"event_cursor", cursor}, {"path", "source.swift"}}),
                 resolved);
        QVERIFY(!f.call("session.resource.resolve", "forged",
                        {{"event_cursor", cursor}, {"path", "other.swift"}})
                     .value("ok")
                     .toBool());
        const auto legacy = f.journal->appendEvent(
            "conversation", 1, "item.completed", {{"item", QJsonObject{{"path", "source.swift"}}}});
        QVERIFY(!f.call("session.resource.resolve", "legacy",
                        {{"event_cursor", legacy}, {"path", "source.swift"}})
                     .value("ok")
                     .toBool());
    }

    void fileOutsideTurnWorktreeResolvesOnlyFromRecordedEvent() {
        Fixture f;
        QVERIFY(f.initialize());
        const auto foreign = f.directory.filePath("foreign");
        QVERIFY(QDir().mkpath(foreign));
        QVERIFY(git(foreign, {"init", "-q"}));
        for (const auto& root : {f.b, foreign}) {
            const auto path = root + "/source.swift";
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("contents");
            file.close();
            for (const auto& item :
                 {QJsonObject{{"type", "fileChange"},
                              {"changes", QJsonArray{QJsonObject{{"path", path}}}}},
                  QJsonObject{{"type", "mcpToolCall"}, {"tool", "Read"}, {"path", path}}}) {
                const auto cursor = f.journal->appendEvent(
                    "conversation", 1, "item.completed", {{"context_revision", 1}, {"item", item}});
                const auto result = f.call("session.resource.resolve", "outside",
                                           {{"event_cursor", cursor}, {"path", path}});
                QVERIFY(result.value("ok").toBool());
                QCOMPARE(result.value("worktree_path").toString(),
                         QFileInfo(root).canonicalFilePath());
                QCOMPARE(result.value("path").toString(), "source.swift");
                QVERIFY(!f.call("session.resource.resolve", "forged",
                                {{"event_cursor", cursor}, {"path", root + "/secret.txt"}})
                             .value("ok")
                             .toBool());
            }
        }
        QCOMPARE(f.journal->sessions().first().worktreePath, f.a);
    }

    void markdownLinksResolveRecordedCheckoutAfterSwitch() {
        Fixture f;
        QVERIFY(f.initialize());
        QFile local(QDir(f.a).filePath("README.md"));
        QVERIFY(local.open(QIODevice::WriteOnly));
        local.write("local report");
        local.close();
        QVERIFY(f.journal->recordSessionContext(f.journal->sessions().first()));
        const auto foreignPath = f.b + "/report with spaces.md";
        QFile file(foreignPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("report");
        file.close();
        const auto encoded = QString::fromUtf8(QUrl::toPercentEncoding(foreignPath, "/"));
        const auto text = QString("[Report](<%1:42>)\n[JSON](README.md:7)\n[Web](https://"
                                  "example.test/x)\n[Private](.git/config)")
                              .arg(encoded);
        const auto cursor = f.journal->appendEvent(
            "conversation", 1, "item.completed",
            {{"context_revision", 1},
             {"resource_provenance", "unknown"},
             {"item", QJsonObject{{"type", "agentMessage"}, {"text", text}}}});
        QVERIFY(f.move(f.b, 1, "switch-for-link").value("ok").toBool());
        const auto resolved = f.call("session.resource.resolve", "reply-link",
                                     {{"event_cursor", cursor}, {"path", foreignPath}});
        QVERIFY(resolved.value("ok").toBool());
        QCOMPARE(resolved.value("worktree_path").toString(), f.b);
        QCOMPARE(resolved.value("path").toString(), "report with spaces.md");
        const auto relative = f.call("session.resource.resolve", "relative-link",
                                     {{"event_cursor", cursor}, {"path", "README.md"}});
        QVERIFY(relative.value("ok").toBool());
        QCOMPARE(relative.value("worktree_path").toString(), f.a);
        QVERIFY(!f.call("session.resource.resolve", "unlinked",
                        {{"event_cursor", cursor}, {"path", "source.swift"}})
                     .value("ok")
                     .toBool());
        QVERIFY(!f.call("session.resource.resolve", "git-private",
                        {{"event_cursor", cursor}, {"path", ".git/config"}})
                     .value("ok")
                     .toBool());
        QVERIFY(f.move(f.a, 2, "switch-back-link").value("ok").toBool());
        QCOMPARE(f.call("session.resource.resolve", "reply-again",
                        {{"event_cursor", cursor}, {"path", foreignPath}}),
                 resolved);
        const auto legacy = f.journal->appendEvent(
            "conversation", 1, "item.completed",
            {{"item", QJsonObject{{"type", "agentMessage"}, {"text", text}}}});
        QVERIFY(!f.call("session.resource.resolve", "unknown-context",
                        {{"event_cursor", legacy}, {"path", foreignPath}})
                     .value("ok")
                     .toBool());
    }

    void recordedActivityIsScopedDurableAndRankedBeforePaging() {
        Fixture f;
        QVERIFY(f.initialize());
        QVERIFY(f.journal->touchedWorktreePaths("conversation", 1).isEmpty());
        QVERIFY(f.move(f.b, 1, "select-only").value("ok").toBool());
        QVERIFY(f.journal->touchedWorktreePaths("conversation", 1).isEmpty());
        QVERIFY(f.move(f.a, 2, "back").value("ok").toBool());
        QVERIFY(f.call("session.start_turn", "start",
                       {{"prompt", "hello"}, {"expected_context_revision", 3}})
                    .value("ok")
                    .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(f.journal->sessions().first().state, "ready", 5000);
        QVERIFY(f.journal->touchedWorktreePaths("conversation", 1).contains(f.a));
        QVERIFY(!f.journal->touchedWorktreePaths("conversation", 1).contains(f.b));
        const auto thread = f.journal->sessions().first().threadId;
        f.provider->notificationReceived("item/completed",
                                         {{"threadId", thread},
                                          {"turnId", "turn-1"},
                                          {"item", QJsonObject{{"id", "command-cross-root"},
                                                               {"type", "commandExecution"},
                                                               {"cwd", f.b},
                                                               {"command", "pwd"}}}});
        QVERIFY(f.journal->touchedWorktreePaths("conversation", 1).contains(f.b));
        QVERIFY(f.journal->touchedWorktreePaths("other-conversation", 1).isEmpty());
        QVERIFY(f.journal->touchedWorktreePaths("conversation", 2).isEmpty());
        const auto page =
            f.call("session.worktrees", "search", {{"query", "feature"}, {"limit", 1}});
        QCOMPARE(page.value("total").toInt(), 1);
        QVERIFY(
            page.value("worktrees").toArray().first().toObject().value("model_touched").toBool());
        QCOMPARE(f.call("session.worktrees", "empty-page", {{"offset", 2}, {"limit", 1}})
                     .value("worktrees")
                     .toArray()
                     .size(),
                 0);
        f.host.reset();
        f.journal.reset();
        f.journal = std::make_unique<LocalJournal>(f.directory.filePath("journal.sqlite"));
        QVERIFY(f.journal->touchedWorktreePaths("conversation", 1).contains(f.b));
    }

    void discoversAndSwitchesRepositoryWithRevisionFence() {
        Fixture f;
        QVERIFY(f.initialize());
        const auto list = f.call("session.worktrees", "list");
        QCOMPARE(list.value("worktrees").toArray().size(), 2);
        QCOMPARE(list.value("context_revision").toInteger(), 1);
        QCOMPARE(f.journal->sessions().first().worktreePath, f.a);
        const auto foreign = f.directory.filePath("foreign");
        QVERIFY(QDir().mkpath(foreign));
        QVERIFY(git(foreign, {"init", "-b", "main"}));
        QVERIFY(f.move(foreign, 1, "foreign").value("ok").toBool());
        QCOMPARE(f.journal->sessions().first().worktreePath,
                 QFileInfo(foreign).canonicalFilePath());
        QVERIFY(f.move(f.a, 2, "back").value("ok").toBool());
        QVERIFY(QDir().mkpath(f.b + "/child"));
        QCOMPARE(f.move(f.b + "/child", 3, "child").value("error_code").toString(),
                 "invalid_worktree");
        QCOMPARE(f.move(f.b + "/missing", 3, "missing").value("error_code").toString(),
                 "invalid_worktree");
        QVERIFY(f.move(f.b, 3, "switch").value("ok").toBool());
        QCOMPARE(f.move(f.a, 1, "stale").value("error_code").toString(), "stale_context");
        QCOMPARE(f.journal->sessions().first().worktreePath, f.b);
    }

    // ADR 0105: the desktop picker lists another repository's worktrees before choosing one.
    void listsAnotherRepositoryWithoutSwitchingOrEnrollingIt() {
        Fixture f;
        QVERIFY(f.initialize());
        auto foreign = f.directory.filePath("foreign");
        const auto foreignFeature = f.directory.filePath("foreign-feature");
        QVERIFY(QDir().mkpath(foreign));
        QVERIFY(git(foreign, {"init", "-b", "main"}));
        QVERIFY(git(foreign,
                    {"-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "-c",
                     "commit.gpgsign=false", "commit", "--allow-empty", "-m", "fixture"}));
        QVERIFY(git(foreign, {"worktree", "add", "-b", "pilot", foreignFeature}));
        foreign = QFileInfo(foreign).canonicalFilePath();

        const auto page = f.call("session.worktrees", "foreign", {{"repository_path", foreign}});
        QVERIFY(page.value("ok").toBool());
        QCOMPARE(page.value("repository_path").toString(), foreign);
        const auto entries = page.value("worktrees").toArray();
        QCOMPARE(entries.size(), 2);
        int mains = 0;
        for (const auto& value : entries) {
            const auto entry = value.toObject();
            if (entry.value("main").toBool()) {
                ++mains;
                QCOMPARE(entry.value("worktree_path").toString(), foreign);
                QCOMPARE(entry.value("branch").toString(), QStringLiteral("main"));
            }
        }
        QCOMPARE(mains, 1);
        // Listing switches nothing and does not enrol the repository.
        QCOMPARE(page.value("worktree_path").toString(), f.a);
        QCOMPARE(page.value("context_revision").toInteger(), 1);
        QCOMPARE(f.journal->sessions().first().worktreePath, f.a);
        QVERIFY(!page.value("repositories").toArray().contains(foreign));
        const auto own = f.call("session.worktrees", "own");
        QCOMPARE(own.value("repository_path").toString(), f.a);
        QVERIFY(!own.value("repositories").toArray().contains(foreign));

        const auto plain = f.directory.filePath("plain");
        QVERIFY(QDir().mkpath(plain));
        QCOMPARE(f.call("session.worktrees", "plain", {{"repository_path", plain}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("invalid_repository"));
        QCOMPARE(f.call("session.worktrees", "missing",
                        {{"repository_path", f.directory.filePath("missing")}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("invalid_repository"));

        // Choosing one of the listed worktrees switches as before.
        QVERIFY(
            f.move(QFileInfo(foreignFeature).canonicalFilePath(), 1, "use").value("ok").toBool());
        QCOMPARE(f.journal->sessions().first().worktreePath,
                 QFileInfo(foreignFeature).canonicalFilePath());
    }
    void switchIsDurableIdempotentAndDoesNotMoveFiles() {
        Fixture f;
        QVERIFY(f.initialize());
        QFile dirty(f.a + "/untracked.txt");
        QVERIFY(dirty.open(QIODevice::WriteOnly));
        dirty.write("keep on A");
        dirty.close();
        const auto first = f.move(f.b, 1, "operation");
        QVERIFY(first.value("ok").toBool());
        QCOMPARE(f.move(f.b, 1, "operation"), first);
        QCOMPARE(f.journal->eventsAfter("conversation", 1, 0, 20).size(), 1);
        f.host.reset();
        f.journal.reset();
        f.journal = std::make_unique<LocalJournal>(f.directory.filePath("journal.sqlite"));
        f.host = std::make_unique<SessionHost>(f.journal.get(), f.provider.get());
        QCOMPARE(f.move(f.b, 1, "operation"), first);
        QVERIFY(f.move(f.a, 2, "back").value("ok").toBool());
        const auto stored = f.journal->sessions().first();
        QCOMPARE(stored.id, "conversation");
        QCOMPARE(stored.generation, 1);
        QCOMPARE(stored.contextRevision, 3);
        QCOMPARE(stored.worktreePath, f.a);
        QCOMPARE(f.journal->conversationPath(stored), f.a);
        const auto history = f.journal->sessionContexts("conversation", 1);
        QCOMPARE(history.size(), 3);
        QCOMPARE(history[0].toObject().value("worktree_path").toString(), f.a);
        QCOMPARE(history[1].toObject().value("worktree_path").toString(), f.b);
        QCOMPARE(history[2].toObject().value("worktree_path").toString(), f.a);
        QVERIFY(QFileInfo::exists(f.a + "/untracked.txt"));
        QVERIFY(!QFileInfo::exists(f.b + "/untracked.txt"));
    }

    void sameProviderThreadReceivesNewCwdAndLegacySendIsFenced_data() {
        QTest::addColumn<bool>("anotherRepository");
        QTest::newRow("linked-worktree") << false;
        QTest::newRow("another-repository") << true;
    }
    void sameProviderThreadReceivesNewCwdAndLegacySendIsFenced() {
        QFETCH(bool, anotherRepository);
        Fixture f;
        QVERIFY(f.initialize());
        if (anotherRepository) {
            f.b = f.directory.filePath("another-repository");
            QVERIFY(QDir().mkpath(f.b));
            QVERIFY(git(f.b, {"init", "-b", "main"}));
            f.b = QFileInfo(f.b).canonicalFilePath();
        }
        QVERIFY(f.call("session.start_turn", "turn-a", {{"prompt", "hello"}}).value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(f.journal->sessions().first().state, "ready", 5000);
        const auto thread = f.journal->sessions().first().threadId;
        QVERIFY(!thread.isEmpty());
        QVERIFY(f.move(f.b, 1, "switch-b").value("ok").toBool());
        QCOMPARE(f.call("session.start_turn", "legacy", {{"prompt", "old client"}})
                     .value("error_code")
                     .toString(),
                 "stale_context");
        QVERIFY(f.call("session.start_turn", "turn-b",
                       {{"prompt", "hello"}, {"expected_context_revision", 2}})
                    .value("ok")
                    .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(f.journal->sessions().first().state, "ready", 5000);
        QCOMPARE(f.journal->sessions().first().threadId, thread);
        QVERIFY(f.move(f.a, 2, "switch-a").value("ok").toBool());
        QVERIFY(f.call("session.start_turn", "turn-a2",
                       {{"prompt", "hello"}, {"expected_context_revision", 3}})
                    .value("ok")
                    .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(f.journal->sessions().first().state, "ready", 5000);
        QCOMPARE(f.journal->sessions().first().threadId, thread);
        const auto encodedPath = [](const QString& path) {
            auto json = QJsonDocument(QJsonArray{path}).toJson(QJsonDocument::Compact);
            return json.mid(1, json.size() - 2);
        };
        QCOMPARE(f.output().count("cwd:" + encodedPath(f.a)), 2);
        QCOMPARE(f.output().count("cwd:" + encodedPath(f.b)), 1);
        QList<qint64> contexts;
        for (const auto& entry : f.journal->eventsAfter("conversation", 1, 0, 100)) {
            const auto event = entry.toObject();
            if (event.value("event_type") == "user.message")
                contexts.append(
                    event.value("payload").toObject().value("context_revision").toInteger());
        }
        QCOMPARE(contexts, (QList<qint64>{1, 2, 3}));
        // Each move names the branch and the folder it went to, never the folder's path.
        QStringList folders;
        for (const auto& entry : f.journal->eventsAfter("conversation", 1, 0, 100)) {
            const auto event = entry.toObject();
            if (event.value("event_type") != "conversation.context_changed")
                continue;
            const auto payload = event.value("payload").toObject();
            QVERIFY(!payload.value("branch").toString().isEmpty());
            QVERIFY(!payload.value("branch").toString().startsWith("refs/"));
            folders.append(payload.value("worktree_name").toString());
            const auto encoded = QJsonDocument(payload).toJson(QJsonDocument::Compact);
            QVERIFY(!encoded.contains(encodedPath(f.a)) && !encoded.contains(encodedPath(f.b)));
        }
        QCOMPARE(folders, (QStringList{QFileInfo(f.b).fileName(), QFileInfo(f.a).fileName()}));
    }

    void newProviderEventsKeepTheirOriginAcrossSwitchAndRestart() {
        Fixture f;
        QVERIFY(f.initialize());
        QFile file(QDir(f.a).filePath("origin.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("A");
        file.close();
        QVERIFY(f.call("session.start_turn", "first", {{"prompt", "hello"}}).value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(f.journal->sessions().first().state, "ready", 5000);
        const auto thread = f.journal->sessions().first().threadId;
        QCOMPARE(f.journal->turnContextRevision("conversation", 1, "turn-1"), 1);
        QVERIFY(f.move(f.b, 1, "move").value("ok").toBool());
        f.host.reset();
        f.host = std::make_unique<SessionHost>(f.journal.get(), f.provider.get());
        auto emitItem = [&](const QString& turn, const QString& path) {
            const QJsonObject item{{"id", "late-file"},
                                   {"type", "fileChange"},
                                   {"changes", QJsonArray{QJsonObject{{"path", path}}}}};
            f.provider->notificationReceived(
                "item/completed", {{"threadId", thread}, {"turnId", turn}, {"item", item}});
            return f.journal->eventsAfter("conversation", 1, 0, 100)
                .last()
                .toObject()
                .value("payload")
                .toObject();
        };
        const auto explicitPath = emitItem("turn-1", file.fileName());
        QCOMPARE(explicitPath.value("context_revision").toInteger(), 1);
        QCOMPARE(explicitPath.value("resource_provenance").toString(), "explicit_path");
        QCOMPARE(explicitPath.value("resource_refs")
                     .toArray()
                     .first()
                     .toObject()
                     .value("path")
                     .toString(),
                 "origin.txt");
        QCOMPARE(emitItem("turn-1", "origin.txt").value("resource_provenance").toString(),
                 "unknown");
        const auto unknown = emitItem("legacy-turn", file.fileName());
        QCOMPARE(unknown.value("provenance").toString(), "unknown");
        QVERIFY(!unknown.contains("context_revision"));
    }

    void deletedWorktreeKeepsWorkingAndFollowsItsReplacement() {
        Fixture f;
        QVERIFY(f.initialize());
        QVERIFY(f.move(f.b, 1, "select-feature").value("ok").toBool());
        QVERIFY(f.call("session.worktrees", "remember").value("ok").toBool());
        QVERIFY(git(f.a, {"worktree", "remove", f.b}));
        const auto page = f.call("session.worktrees", "after-delete");
        QVERIFY(page.value("ok").toBool());
        QVERIFY(page.value("switch_available").toBool());
        QCOMPARE(page.value("worktrees").toArray().size(), 1);
        // The turn runs in the primary worktree; the worktree it adds becomes the context.
        QVERIFY(f.call("session.start_turn", "fallback",
                       {{"prompt", "slow-start"},
                        {"worktree_path", f.b},
                        {"expected_context_revision", 2}})
                    .value("ok")
                    .toBool());
        QCOMPARE(f.journal->sessions().first().worktreePath, f.b);
        QTRY_VERIFY_WITH_TIMEOUT(f.output().contains("cwd:\"" + f.a.toUtf8() + "\""), 5000);
        const auto replacement = f.directory.filePath("feature-again");
        QVERIFY(git(f.a, {"worktree", "add", "-b", "feature-again", replacement}));
        QTRY_COMPARE_WITH_TIMEOUT(f.journal->sessions().first().contextRevision, 3, 5000);
        QCOMPARE(f.journal->sessions().first().worktreePath,
                 QFileInfo(replacement).canonicalFilePath());
        QTRY_COMPARE_WITH_TIMEOUT(f.journal->sessions().first().state, QString("ready"), 5000);
        QVERIFY(f.call("session.start_turn", "continued",
                       {{"prompt", "hello"}, {"expected_context_revision", 3}})
                    .value("ok")
                    .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(f.journal->sessions().first().state, QString("ready"), 5000);
    }

    void runningTurnCanChooseNextWorktree_data() {
        QTest::addColumn<bool>("anotherRepository");
        QTest::newRow("linked-worktree") << false;
        QTest::newRow("another-repository") << true;
    }
    void runningTurnCanChooseNextWorktree() {
        QFETCH(bool, anotherRepository);
        Fixture f;
        QVERIFY(f.initialize());
        if (anotherRepository) {
            f.b = f.directory.filePath("running-target");
            QVERIFY(QDir().mkpath(f.b));
            QVERIFY(git(f.b, {"init", "-b", "next"}));
            f.b = QFileInfo(f.b).canonicalFilePath();
        }
        QVERIFY(f.call("session.start_turn", "start", {{"prompt", "hang"}}).value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(f.journal->sessions().first().state, QString("running"), 5000);
        QVERIFY(f.call("session.worktrees", "running-list").value("switch_available").toBool());
        QVERIFY(f.move(f.b, 1, "next-context").value("ok").toBool());
        QCOMPARE(f.journal->sessions().first().state, QString("running"));
        QCOMPARE(f.journal->sessions().first().worktreePath, f.b);
        const auto turnId = f.journal->sessions().first().turnId;
        QVERIFY(!turnId.isEmpty());
        QCOMPARE(f.journal->turnContextRevision("conversation", 1, turnId), 1);
    }

    void startingAndPendingApprovalCannotSwitch() {
        Fixture f;
        QVERIFY(f.initialize());
        QVERIFY(
            f.call("session.start_turn", "start", {{"prompt", "approve"}}).value("ok").toBool());
        QCOMPARE(f.move(f.b, 1, "starting").value("error_code").toString(), "session_busy");
        QTRY_VERIFY_WITH_TIMEOUT(!f.call("session.pending_approvals", "pending")
                                      .value("pending_approvals")
                                      .toArray()
                                      .isEmpty(),
                                 5000);
        QCOMPARE(f.move(f.b, 1, "waiting").value("error_code").toString(), "session_busy");
        QCOMPARE(f.journal->sessions().first().worktreePath, f.a);
    }

    void contextCommitRollsBackWhenDurableResultCannotBeStored() {
        Fixture f;
        QVERIFY(f.initialize());
        const auto name = QStringLiteral("context-failure-test");
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", name);
            database.setDatabaseName(f.journal->databasePath());
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(
                "CREATE TRIGGER fail_result BEFORE INSERT ON command_results "
                "WHEN NEW.message_id = 'fail' BEGIN SELECT RAISE(ABORT, 'fixture'); END"));
        }
        QSqlDatabase::removeDatabase(name);
        QCOMPARE(f.move(f.b, 1, "fail").value("error_code").toString(), "journal_error");
        QCOMPARE(f.journal->sessions().first().contextRevision, 1);
        QCOMPARE(f.journal->sessions().first().worktreePath, f.a);
        QCOMPARE(f.journal->sessionContexts("conversation", 1).size(), 1);
        QVERIFY(f.journal->eventsAfter("conversation", 1, 0, 20).isEmpty());
        QVERIFY(f.journal->commandResult("fail").value("ok") != true);
    }

    void legacyJournalDoesNotBackfillAndStaleWriterCannotUndoSwitch() {
        Fixture f;
        QVERIFY(f.initialize());
        f.host.reset();
        f.journal.reset();
        const auto name = QStringLiteral("context-migration-test");
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", name);
            database.setDatabaseName(f.directory.filePath("journal.sqlite"));
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec("DROP TABLE session_contexts"));
            QVERIFY(query.exec("ALTER TABLE sessions DROP COLUMN context_revision"));
        }
        QSqlDatabase::removeDatabase(name);
        f.journal = std::make_unique<LocalJournal>(f.directory.filePath("journal.sqlite"));
        QVERIFY(f.journal->isOpen());
        const auto original = f.journal->sessions().first();
        QCOMPARE(original.contextRevision, 1);
        QVERIFY(f.journal->sessionContexts("conversation", 1).isEmpty());
        f.host = std::make_unique<SessionHost>(f.journal.get(), f.provider.get());
        QVERIFY(f.move(f.b, 1, "switch").value("ok").toBool());
        QVERIFY(!f.journal->upsertSession(original));
        QCOMPARE(f.journal->sessions().first().contextRevision, 2);
        QCOMPARE(f.journal->sessions().first().worktreePath, f.b);
    }

    void selectingCurrentWorktreeDoesNotCreateANewContext() {
        Fixture f;
        QVERIFY(f.initialize());
        const auto result = f.move(f.a, 1, "same");
        QVERIFY(result.value("ok").toBool());
        QCOMPARE(result.value("context_revision").toInteger(), 1);
        QCOMPARE(f.journal->sessionContexts("conversation", 1).size(), 1);
        QVERIFY(f.journal->eventsAfter("conversation", 1, 0, 20).isEmpty());
    }

    void staleSteerIsFencedButApprovalStillWorksAfterSwitch() {
        Fixture f;
        QVERIFY(f.initialize());
        QVERIFY(f.move(f.b, 1, "switch").value("ok").toBool());
        QCOMPARE(f.call("session.steer", "old-steer", {{"prompt", "old scope"}})
                     .value("error_code")
                     .toString(),
                 "stale_context");
        QCOMPARE(f.call("session.steer", "valid-steer",
                        {{"prompt", "new scope"}, {"expected_context_revision", 2}})
                     .value("error_code")
                     .toString(),
                 "turn_not_running");
        QVERIFY(f.call("session.start_turn", "start",
                       {{"prompt", "approve"}, {"expected_context_revision", 2}})
                    .value("ok")
                    .toBool());
        QJsonArray pending;
        QTRY_VERIFY_WITH_TIMEOUT(!(pending = f.call("session.pending_approvals", "pending")
                                                 .value("pending_approvals")
                                                 .toArray())
                                      .isEmpty(),
                                 5000);
        const auto approval = pending.first().toObject().value("approval_id");
        const auto result =
            f.host->handleCommand(request("approval.resolve", "resolve",
                                          {{"approval_id", approval},
                                           {"resolution", QJsonObject{{"decision", "accept"}}}}),
                                  "cloud-control-v2");
        QVERIFY(result.value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(f.journal->sessions().first().state, "ready", 5000);
        QCOMPARE(f.journal->sessions().first().contextRevision, 2);
    }

    void providerCatalogIsSharedAndAccountChangesInvalidateIt() {
        Fixture f;
        QVERIFY(f.initialize());
        QFile executable(f.directory.filePath("fake-codex"));
        QVERIFY(executable.open(QIODevice::ReadOnly));
        auto script = executable.readAll();
        executable.close();
        script.replace("elif method == 'model/list':\n",
                       "elif method == 'model/list':\n        log('model-list')\n");
        QVERIFY(executable.open(QIODevice::WriteOnly | QIODevice::Truncate));
        executable.write(script);
        executable.close();
        int replies = 0;
        const auto accept = [&](const QJsonObject& result, const QString& error) {
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QVERIFY(!result.value("catalog").toArray().isEmpty());
            ++replies;
        };
        f.host->handleProviderQuery(
            {{"message_type", "provider.configuration_catalog"}, {"session_id", "conversation"}},
            accept);
        QTRY_COMPARE(replies, 1);
        QCOMPARE(f.output().count("model-list"), 1);
        f.host->handleProviderQuery({{"message_type", "provider.configuration_catalog"}}, accept);
        QCOMPARE(replies, 2); // Synchronous cache response even from another scope.
        QCOMPARE(f.output().count("model-list"), 1);
        f.provider->notificationReceived("account/updated", {});
        f.host->handleProviderQuery({{"message_type", "provider.configuration_catalog"}}, accept);
        QTRY_COMPARE(replies, 3);
        QCOMPARE(f.output().count("model-list"), 2);
    }

    void desktopAndPhoneShareConfigurationWithoutResettingPermission() {
        Fixture f;
        QVERIFY(f.initialize());
        const auto desktop = f.call("session.configuration.apply", "desktop",
                                    {{"desktop_selection", true},
                                     {"expected_revision", 1},
                                     {"model", "fixture-model"},
                                     {"effort", "low"},
                                     {"execution_policy", "full"},
                                     {"risk_acknowledged", true}});
        QVERIFY(desktop.value("ok").toBool());
        QCOMPARE(desktop.value("revision").toInteger(), 2);
        QVERIFY(f.move(f.b, 1, "switch-settings").value("ok").toBool());
        const auto read = f.host->handleCommand(
            request("session.configuration.read", "phone-read", {{"resource_context_revision", 2}}),
            "cloud-control-v2");
        QCOMPARE(read.value("effective").toObject().value("execution_policy").toString(), "full");
        QCOMPARE(read.value("effective").toObject().value("model").toString(), "fixture-model");
        const auto light = f.host->handleCommand(
            request("session.configuration.read", "phone-light",
                    {{"resource_context_revision", 2}, {"effective_only", true}}),
            "cloud-control-v2");
        QVERIFY(light.value("effective_only").toBool());
        QCOMPARE(light.value("effective"), read.value("effective"));
        QVERIFY(!light.contains("models"));
        QVERIFY(!light.contains("catalog_revision"));

        const auto model = f.call("session.configuration.apply", "desktop-model",
                                  {{"desktop_selection", true},
                                   {"expected_revision", 2},
                                   {"model", "another-model"},
                                   {"effort", "high"}});
        QVERIFY(model.value("ok").toBool());
        QCOMPARE(model.value("effective").toObject().value("execution_policy").toString(), "full");
        const auto mobile =
            f.host->handleCommand(request("session.execution_policy.apply", "phone-policy",
                                          {{"resource_context_revision", 2},
                                           {"expected_revision", 3},
                                           {"execution_policy", "ask"}}),
                                  "cloud-control-v2");
        QVERIFY(mobile.value("ok").toBool());
        const auto listed =
            f.call("session.list", "list-settings").value("sessions").toArray().first().toObject();
        QCOMPARE(listed.value("configuration_revision").toInteger(), 4);
        QCOMPARE(listed.value("effective").toObject().value("execution_policy").toString(), "ask");
        const auto stale =
            f.call("session.configuration.apply", "stale-desktop",
                   {{"desktop_selection", true}, {"expected_revision", 3}, {"model", "old"}});
        QCOMPARE(stale.value("error_code").toString(), "stale_configuration");
        const auto attack =
            f.host->handleCommand(request("session.configuration.apply", "remote-catalog-bypass",
                                          {{"desktop_selection", true},
                                           {"resource_context_revision", 2},
                                           {"expected_revision", 4},
                                           {"model", "unknown"}}),
                                  "cloud-control-v2");
        QCOMPARE(attack.value("error_code").toString(), "stale_model_catalog");
        f.host = std::make_unique<SessionHost>(f.journal.get(), f.provider.get());
        const auto restored = f.call("session.configuration.read", "after-restart");
        QCOMPARE(restored.value("revision").toInteger(), 4);
        QCOMPARE(restored.value("effective").toObject().value("model").toString(), "another-model");
    }

    void relaySettingsAndAttachmentsRequireCurrentContext() {
        Fixture f;
        QVERIFY(f.initialize());
        QVERIFY(f.move(f.b, 1, "switch-settings").value("ok").toBool());
        const QStringList types{"session.configuration.read",     "session.configuration.apply",
                                "session.execution_policy.apply", "session.mode.apply",
                                "attachment.stage.begin",         "attachment.stage.chunk",
                                "attachment.stage.commit",        "attachment.stage.cancel"};
        for (const auto& type : types) {
            for (const auto revision : {0, 1, 2}) {
                QJsonObject payload;
                if (revision)
                    payload.insert("resource_context_revision", revision);
                const auto result = f.host->handleCommand(
                    request(type, type + QString::number(revision), payload), "cloud-control-v2");
                if (revision == 2) {
                    QVERIFY2(result.value("error_code") != "context_upgrade_required",
                             qPrintable(type));
                    if (type == "session.configuration.read")
                        QVERIFY(result.value("ok").toBool());
                } else
                    QCOMPARE(result.value("error_code").toString(), "context_upgrade_required");
            }
        }
    }

    void transcriptReadsAreNotFencedByTheCurrentWorktree() {
        Fixture f;
        QVERIFY(f.initialize());
        QVERIFY(f.move(f.b, 1, "switch-reads")["ok"].toBool());
        for (const QString& kind :
             {QString("session.output.read"), QString("session.media.read")}) {
            const auto result = f.host->handleCommand(request(kind, kind,
                                                              {{"item_id", "missing"},
                                                               {"turn_sequence", 1},
                                                               {"media_ref", "sha256:missing"}}),
                                                      "cloud-control-v2");
            // A missing historical resource gets its own error, never a context/offline error.
            QVERIFY(!result["ok"].toBool());
            QVERIFY(result["error_code"] != "context_upgrade_required");
        }
        const auto pathRead = f.host->handleCommand(
            request("session.media.read", "path-read", {{"path", "picture.png"}}),
            "cloud-control-v2");
        QCOMPARE(pathRead["error_code"].toString(), QString("context_upgrade_required"));
    }

    void relayRequiresContextFenceAfterSwitch() {
        Fixture f;
        QVERIFY(f.initialize());
        QVERIFY(f.move(f.b, 1, "local-switch").value("ok").toBool());
        const auto command = request("session.start_turn", "remote-start", {{"prompt", "hello"}});
        const auto result = f.host->handleCommand(command, "cloud-control-v2");
        QCOMPARE(result.value("error_code").toString(), "context_upgrade_required");
        QVERIFY(f.journal->sessions().first().threadId.isEmpty());
        QCOMPARE(
            f.host
                ->handleCommand(request("session.pending_approvals", "pending"), "cloud-control-v2")
                .value("ok")
                .toBool(),
            true);
    }
};

QTEST_GUILESS_MAIN(DaemonWorktreeContextTest)
#include "DaemonWorktreeContextTest.moc"
