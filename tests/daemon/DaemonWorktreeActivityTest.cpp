#include "FakeCodex.h"
#include "TestPaths.h"
#include "ipc/LocalIpcServer.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace ketplus::daemon;

namespace {

bool git(const QString& root, const QStringList& args) {
    QProcess process;
    process.start("git", QStringList{"-C", root} + args);
    return process.waitForFinished(10000) && process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0;
}

QJsonObject request(const QString& type, const QJsonObject& payload = {}) {
    return {{"protocol_version", protocol::currentVersion},
            {"message_type", type},
            {"message_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"session_id", "conversation"},
            {"session_generation", 1},
            {"sent_at", protocol::timestamp()},
            {"payload", payload}};
}

QByteArray quoted(const QString& path) { return "'" + path.toUtf8().replace("'", "'\\''") + "'"; }

// A git first on PATH that records each call and, given a gate, waits for that file before it
// runs the real git: a barrier the test opens, not a delay.
struct GitWrapper final {
    QByteArray path = qgetenv("PATH");
    QString calls;
    QString started;
    QString gate;
    bool installed = false;

    GitWrapper(const QString& directory, bool gated)
        : calls(directory + "/calls.log"), started(directory + "/started"),
          gate(gated ? directory + "/gate" : QString{}) {
        const QString real = QStandardPaths::findExecutable("git");
        QFile script(directory + "/git");
        if (real.isEmpty() || !QDir().mkpath(directory) || !script.open(QIODevice::WriteOnly))
            return;
        script.write("#!/bin/sh\nprintf '%s\\n' \"$*\" >> " + quoted(calls) + "\n");
        if (gated)
            script.write(": > " + quoted(started) + "\nwhile [ ! -f " + quoted(gate) +
                         " ]; do sleep 0.01; done\n");
        script.write("exec " + quoted(real) + " \"$@\"\n");
        script.close();
        installed = script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("PATH", directory.toUtf8() + ':' + path);
    }
    bool open() const {
        if (gate.isEmpty())
            return true;
        QFile file(gate);
        return file.open(QIODevice::WriteOnly);
    }
    QByteArray log() const {
        QFile file(calls);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    }
    ~GitWrapper() {
        // A failed assertion must still let a waiting worker finish before the host joins it.
        (void)open();
        qputenv("PATH", path);
    }
    GitWrapper(const GitWrapper&) = delete;
    GitWrapper& operator=(const GitWrapper&) = delete;
};

struct Fixture {
    QTemporaryDir directory;
    QString a{directory.filePath("repo")};
    QString b{directory.filePath("feature")};
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
        const auto executable = directory.filePath(fixtureName("fake-codex"));
        if (!writeFakeCodex(executable, directory.filePath("provider.log")))
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
    QJsonObject call(const QString& type, const QJsonObject& payload = {}) {
        return host->handleCommand(request(type, payload), "worktree-activity-test");
    }
    // A finished turn, so the tool calls below carry its context revision.
    bool finishTurn() {
        if (!call("session.start_turn", {{"prompt", "hello"}}).value("ok").toBool())
            return false;
        QElapsedTimer timer;
        timer.start();
        while (journal->sessions().first().state != "ready" && timer.elapsed() < 10000)
            QTest::qWait(10);
        return journal->sessions().first().state == "ready";
    }
    void tool(const QString& method, const QJsonObject& item) {
        provider->notificationReceived(method, {{"threadId", journal->sessions().first().threadId},
                                                {"turnId", "turn-1"},
                                                {"item", item}});
    }
    void command(const QString& cwd) {
        const QJsonObject item{{"id", QUuid::createUuid().toString()},
                               {"type", "commandExecution"},
                               {"status", "completed"},
                               {"exitCode", 0},
                               {"cwd", cwd},
                               {"command", "pwd"}};
        tool("item/started", item);
        tool("item/completed", item);
    }
    QSet<QString> touched() const { return journal->touchedWorktreePaths("conversation", 1); }
};

class Client final {
  public:
    QLocalSocket socket;
    void send(const QJsonObject& request) {
        socket.write(protocol::encodeFrame(request));
        socket.flush();
    }
    // Every answer read so far, by the request it answers.
    QHash<QString, QJsonObject> answers;
    void read() {
        buffer_.append(socket.readAll());
        while (buffer_.size() >= 4) {
            const auto length = (quint32(uchar(buffer_[0])) << 24) |
                                (quint32(uchar(buffer_[1])) << 16) |
                                (quint32(uchar(buffer_[2])) << 8) | quint32(uchar(buffer_[3]));
            if (buffer_.size() < length + 4)
                break;
            const auto frame = QJsonDocument::fromJson(buffer_.mid(4, length)).object();
            buffer_.remove(0, length + 4);
            answers.insert(frame.value("correlation_id").toString(),
                           frame.value("payload").toObject());
        }
    }
    bool answered(const QJsonObject& request) {
        read();
        return answers.contains(request.value("message_id").toString());
    }

  private:
    QByteArray buffer_;
};

} // namespace

class DaemonWorktreeActivityTest final : public QObject {
    Q_OBJECT
  private slots:
    // A tool call's worktree is matched against Git's own files: no git process runs for it.
    void recordsToolCallsWithoutRunningGit() {
        KETPLUS_SKIP_ON_WINDOWS("the git wrapper is a POSIX shell script");
        Fixture f;
        QVERIFY(f.initialize());
        QVERIFY(f.finishTurn());
        QVERIFY(QDir().mkpath(f.b + "/src"));
        QFile changed(f.b + "/src/changed.txt");
        QVERIFY(changed.open(QIODevice::WriteOnly));
        changed.close();
        QVERIFY(!f.touched().contains(f.b));

        const GitWrapper wrapper(f.directory.filePath("bin"), false);
        QVERIFY(wrapper.installed);
        f.command(f.b + "/src");
        QVERIFY(f.touched().contains(f.b));
        const QJsonObject file{{"id", "file"},
                               {"type", "fileChange"},
                               {"status", "completed"},
                               {"changes", QJsonArray{QJsonObject{{"path", changed.fileName()}}}}};
        f.tool("item/started", file);
        f.tool("item/completed", file);
        // A relative cwd or a path outside every checkout records nothing.
        const auto before = f.touched();
        f.command("feature");
        f.command(f.directory.path());
        QCOMPARE(f.touched(), before);
        QCOMPARE(wrapper.log(), QByteArray{});
    }

    // Adding a linked worktree, or removing one, is seen on the next tool call.
    void followsAddedAndRemovedWorktrees() {
        Fixture f;
        QVERIFY(f.initialize());
        QVERIFY(f.finishTurn());
        // Prime the listing with both checkouts.
        f.command(f.a);
        QVERIFY(f.touched().contains(f.a));
        QVERIFY(!f.touched().contains(f.b));

        // A checkout inside another one is matched by its longest root.
        const auto nestedPath = f.a + "/nested";
        QVERIFY(git(f.a, {"worktree", "add", "-b", "nested", nestedPath}));
        const auto nested = QFileInfo(nestedPath).canonicalFilePath();
        QVERIFY(QDir().mkpath(nested + "/deep"));
        f.command(nested + "/deep");
        QVERIFY(f.touched().contains(nested));

        // A removed worktree no longer matches, even when a plain folder takes its place.
        QVERIFY(git(f.a, {"worktree", "remove", "--force", f.b}));
        QVERIFY(QDir().mkpath(f.b));
        f.command(f.b);
        QVERIFY(!f.touched().contains(f.b));
    }

    // session.worktrees from a local client reads Git off the daemon thread: another command
    // is answered while that git waits, and the listing is the one answered directly.
    void localListingDoesNotHoldOtherCommands() {
        KETPLUS_SKIP_ON_WINDOWS("the git wrapper is a POSIX shell script");
        Fixture f;
        QVERIFY(f.initialize());
        const auto direct = f.call("session.worktrees");
        QVERIFY(direct.value("ok").toBool());
        LocalIpcServer server(f.host.get(), f.directory.filePath("ipc.sock"), "user-token");
        QVERIFY2(server.listen(), qPrintable(server.errorString()));
        Client client;
        client.socket.connectToServer(server.fullServerName());
        QTRY_COMPARE(client.socket.state(), QLocalSocket::ConnectedState);
        const auto hello =
            request("daemon.hello", {{"token", "user-token"}, {"client_id", "test"}});
        client.send(hello);
        QTRY_VERIFY(client.answered(hello));
        QVERIFY(client.answers.value(hello.value("message_id").toString()).value("ok").toBool());

        const GitWrapper wrapper(f.directory.filePath("bin"), true);
        QVERIFY(wrapper.installed);
        const auto listing = request("session.worktrees");
        client.send(listing);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(wrapper.started), 10000);
        const auto other = request("session.list");
        client.send(other);
        QTRY_VERIFY_WITH_TIMEOUT(client.answered(other), 10000);
        QVERIFY(client.answers.value(other.value("message_id").toString()).value("ok").toBool());
        QVERIFY2(!client.answered(listing), "The listing answered before its git was let go.");
        QVERIFY(wrapper.open());
        QTRY_VERIFY_WITH_TIMEOUT(client.answered(listing), 10000);
        QCOMPARE(client.answers.value(listing.value("message_id").toString()), direct);
    }
};

QTEST_GUILESS_MAIN(DaemonWorktreeActivityTest)
#include "DaemonWorktreeActivityTest.moc"
