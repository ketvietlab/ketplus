#include "FakeClaude.h"
#include "protocol/DaemonProtocol.h"
#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include <QDir>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;

namespace {
bool git(const QString& cwd, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(cwd);
    process.start("git", arguments);
    return process.waitForFinished() && process.exitCode() == 0;
}

struct Fixture {
    QTemporaryDir directory;
    QString a{directory.filePath("repo")};
    QString b{directory.filePath("feature")};
    QString log{directory.filePath("claude.log")};
    std::unique_ptr<LocalJournal> journal;
    std::unique_ptr<CodexAdapter> codex;
    std::unique_ptr<ClaudeAdapter> claude;
    std::unique_ptr<SessionHost> host;

    bool initialize() {
        if (!directory.isValid() || !QDir().mkpath(a) || !git(a, {"init", "-b", "main"}) ||
            !git(a, {"-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "-c",
                     "commit.gpgsign=false", "commit", "--allow-empty", "-m", "fixture"}) ||
            !git(a, {"worktree", "add", "-b", "feature", b}))
            return false;
        a = QFileInfo(a).canonicalFilePath();
        b = QFileInfo(b).canonicalFilePath();
        const auto executable = directory.filePath("fake-claude");
        if (!writeFakeClaude(executable, log))
            return false;
        journal = std::make_unique<LocalJournal>(directory.filePath("journal.sqlite"));
        StoredSession session;
        session.id = "conversation";
        session.worktreePath = a;
        session.adapter = "claude";
        session.state = "ready";
        if (!journal->isOpen() || !journal->upsertSession(session))
            return false;
        codex = std::make_unique<CodexAdapter>("/missing-codex-test");
        claude = std::make_unique<ClaudeAdapter>(executable);
        host = std::make_unique<SessionHost>(journal.get(), codex.get(), claude.get());
        return true;
    }

    QJsonObject call(const QString& type, const QString& id, const QJsonObject& payload = {}) {
        return host->handleCommand({{"protocol_version", protocol::currentVersion},
                                    {"message_id", id},
                                    {"message_type", type},
                                    {"session_id", "conversation"},
                                    {"session_generation", 1},
                                    {"sent_at", protocol::timestamp()},
                                    {"payload", payload}},
                                   "isolated-claude-worktree-test");
    }

    QJsonObject move(const QString& path, int revision, const QString& id) {
        return call("session.worktree.switch", id,
                    {{"worktree_path", path}, {"expected_context_revision", revision}});
    }

    QJsonArray launches() const {
        QFile file(log);
        QJsonArray result;
        if (file.open(QIODevice::ReadOnly))
            for (const auto& line : file.readAll().split('\n'))
                if (line.startsWith("setup:"))
                    result.append(QJsonDocument::fromJson(line.mid(6)).object());
        return result;
    }
};
} // namespace

class ClaudeWorktreeTest final : public QObject {
    Q_OBJECT
  private slots:
    void resumesSameConversationAcrossWorktreesAndRestart() {
        Fixture f;
        QVERIFY(f.initialize());
        const auto page = f.call("session.worktrees", "list");
        QVERIFY(page["switch_supported"].toBool());
        QVERIFY(page["switch_available"].toBool());
        QVERIFY(f.call("session.start_turn", "first", {{"prompt", "first"}})["ok"].toBool());
        QTRY_COMPARE(f.journal->sessions().first().state, QString("ready"));
        const auto thread = f.journal->sessions().first().threadId;
        QVERIFY(!thread.isEmpty());
        const auto moved = f.move(f.b, 1, "to-feature");
        QVERIFY(moved["ok"].toBool());
        QCOMPARE(moved["context_revision"].toInt(), 2);
        QCOMPARE(f.move(f.a, 1, "stale")["error_code"].toString(), "stale_context");
        QVERIFY(f.call("session.start_turn", "second",
                       {{"prompt", "second"}, {"expected_context_revision", 2}})["ok"]
                    .toBool());
        QTRY_COMPARE(f.journal->sessions().first().state, QString("ready"));
        QCOMPARE(f.journal->sessions().first().threadId, thread);
        QCOMPARE(f.launches().size(), 2);
        QCOMPARE(f.launches()[1].toObject()["cwd"].toString(), f.b);
        QCOMPARE(f.launches()[1].toObject()["resume"].toString(), thread);

        f.host.reset();
        f.claude->stop();
        f.host = std::make_unique<SessionHost>(f.journal.get(), f.codex.get(), f.claude.get());
        QVERIFY(f.move(f.a, 2, "back")["ok"].toBool());
        QVERIFY(f.call("session.start_turn", "third",
                       {{"prompt", "third"}, {"expected_context_revision", 3}})["ok"]
                    .toBool());
        QTRY_COMPARE(f.journal->sessions().first().state, QString("ready"));
        QCOMPARE(f.journal->sessions().size(), 1);
        QCOMPARE(f.journal->sessions().first().id, "conversation");
        QCOMPARE(f.journal->sessions().first().threadId, thread);
        QCOMPARE(f.launches()[2].toObject()["cwd"].toString(), f.a);
        QCOMPARE(f.launches()[2].toObject()["resume"].toString(), thread);
        f.claude->stop();
    }

    void busyClaudeKeepsItsContext_data() {
        QTest::addColumn<QString>("prompt");
        QTest::addColumn<QString>("state");
        QTest::newRow("running") << QString("hang") << QString("running");
        QTest::newRow("approval") << QString("needs-approval") << QString("running");
        QTest::newRow("background") << QString("background") << QString("ready");
    }
    void busyClaudeKeepsItsContext() {
        QFETCH(QString, prompt);
        QFETCH(QString, state);
        Fixture f;
        QVERIFY(f.initialize());
        QVERIFY(f.call("session.start_turn", "busy", {{"prompt", prompt}})["ok"].toBool());
        QTRY_COMPARE(f.journal->sessions().first().state, state);
        if (prompt == "needs-approval")
            QTRY_COMPARE(f.call("session.pending_approvals", "pending")["pending_approvals"]
                             .toArray()
                             .size(),
                         1);
        const auto page = f.call("session.worktrees", "list-busy");
        QVERIFY(page["switch_supported"].toBool());
        QVERIFY(!page["switch_available"].toBool());
        QCOMPARE(f.move(f.b, 1, "refused")["error_code"].toString(), "session_busy");
        QCOMPARE(f.journal->sessions().first().worktreePath, f.a);
        QCOMPARE(f.journal->sessions().first().contextRevision, 1);
        f.claude->stop();
    }
};

QTEST_GUILESS_MAIN(ClaudeWorktreeTest)
#include "ClaudeWorktreeTest.moc"
