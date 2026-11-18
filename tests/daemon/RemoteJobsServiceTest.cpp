// RemoteJobsService with its real parts on a temporary registry and state directory: Git on the
// thread pool, the snapshot stores, the pin store, the runner links and the sweep timer. Only the
// runner port of the Control client and the Relay socket are played by the test.
#include "runner/remote/RemoteJobsService.h"
#include "RemoteJobLedgerFixture.h"
#include "cloud/CredentialStore.h"
#include "runner/RunnerRelayPort.h"
#include "runner/remote/RemoteJobs.h"
#include "runner/sync/SnapshotPreparation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>
#include <QThreadPool>

using namespace remote_job_ledger_test;
using ketplus::daemon::CredentialStore;
using ketplus::daemon::runner::MachineIdentity;
using ketplus::daemon::runner::RelayReply;
using ketplus::daemon::runner::RunnerRelayPort;
using ketplus::daemon::runner::remote::RelaySocket;
using ketplus::daemon::runner::remote::RemoteJobsService;
using ketplus::daemon::runner::remote::SubmitRequest;
namespace prep = ::ketplus::daemon::runner::sync;
namespace jobs = ::ketplus::daemon::runner::jobs;

namespace {

class FakePort final : public RunnerRelayPort {
  public:
    struct Request {
        QByteArray method;
        QString path;
        QJsonValue body;
        ReplyCallback callback;
    };

    [[nodiscard]] bool runnerRelayAvailable() const override { return available; }
    [[nodiscard]] std::optional<MachineIdentity> machineIdentity() override { return identity; }
    [[nodiscard]] QString accountScope() const override { return scope; }
    [[nodiscard]] QByteArray signAsMachine(const QByteArray&) override {
        return QByteArray(64, 's');
    }
    [[nodiscard]] CredentialStore& credentials() override { return store; }
    void runnerRequest(const QByteArray& method, const QString& path, const QJsonValue& body,
                       ReplyCallback callback) override {
        requests.append({method, path, body, std::move(callback)});
    }

    bool available = true;
    std::optional<MachineIdentity> identity;
    QString scope = QStringLiteral("account-1");
    CredentialStore store{"remote-jobs-service-test"};
    QList<Request> requests;
};

// The socket's side the test sees; it outlives the service that owns the socket.
struct SocketState {
    // What the Control client does with a message: true when the service took it.
    bool deliver(const QJsonObject& message) const { return take && take(message); }

    QList<QJsonObject> sent;
    RelaySocket::Take take;
    std::function<void()> reopened;
};

class FakeSocket final : public RelaySocket {
  public:
    explicit FakeSocket(SocketState& state) : state_(state) {}
    bool send(const QJsonObject& message) override {
        state_.sent.append(message);
        return true;
    }
    void take(Take taker, std::function<void()> reopened) override {
        state_.take = std::move(taker);
        state_.reopened = std::move(reopened);
    }

  private:
    SocketState& state_;
};

MachineIdentity machine() {
    return {QStringLiteral("machine-1"), QStringLiteral("workspace-1"), 1, QByteArray(32, 'k'),
            QByteArray(32, 'a')};
}

const QJsonObject jobChanged{{"type", "job.changed"}, {"jobId", "job-1"}};

bool git(const QString& home, const QString& directory, const QStringList& arguments) {
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("PATH"), qEnvironmentVariable("PATH"));
    environment.insert(QStringLiteral("HOME"), home);
    environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
    for (const auto* name : {"GIT_AUTHOR_NAME", "GIT_COMMITTER_NAME"})
        environment.insert(QString::fromLatin1(name), QStringLiteral("Fixture"));
    for (const auto* name : {"GIT_AUTHOR_EMAIL", "GIT_COMMITTER_EMAIL"})
        environment.insert(QString::fromLatin1(name), QStringLiteral("fixture@example.invalid"));
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(directory);
    process.start(QStringLiteral("git"), arguments);
    return process.waitForFinished(60'000) && process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0;
}

// A repository with one commit in `root`; its worktree directory `src`.
QString repository(const QString& root) {
    const QString home = QDir(root).filePath(QStringLiteral("home"));
    const QString repo = QDir(root).filePath(QStringLiteral("repo"));
    QDir().mkpath(home);
    QDir().mkpath(QDir(repo).filePath(QStringLiteral("src")));
    QFile file(QDir(repo).filePath(QStringLiteral("src/main.c")));
    if (!file.open(QIODevice::WriteOnly) || file.write("int main() { return 0; }\n") < 0)
        return {};
    file.close();
    const bool made =
        git(home, repo, {QStringLiteral("init"), QStringLiteral("-q")}) &&
        git(home, repo, {QStringLiteral("add"), QStringLiteral(".")}) &&
        git(home, repo, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("one")});
    return made ? QDir(repo).filePath(QStringLiteral("src")) : QString();
}

SubmitRequest requestIn(const QString& cwd) {
    SubmitRequest request;
    request.spec.invocation.argv = QStringList{QStringLiteral("make")};
    request.spec.cwd = cwd;
    request.runsOn = linuxLabels;
    request.timeoutMs = 60'000;
    return request;
}

QJsonObject queuedJob() {
    return {{"jobId", "job-1"},
            {"state", "queued"},
            {"attempt", 0},
            {"runnerId", QJsonValue::Null},
            {"queuedAt", "2026-11-18T09:00:00.000Z"},
            {"expiresAt", "2026-11-19T09:00:00.000Z"},
            {"startedAt", QJsonValue::Null},
            {"endedAt", QJsonValue::Null},
            {"outcome", QJsonValue::Null},
            {"reason", QJsonValue::Null}};
}

// The service and the parts the test plays.
struct Service {
    LedgerFixture f;
    QTemporaryDir state;
    FakePort port;
    SocketState socket;
    std::unique_ptr<RemoteJobsService> service;

    explicit Service(const int sweepIntervalMs = RemoteJobsService::hourlySweepMs) {
        service = std::make_unique<RemoteJobsService>(
            *f.ledger, port, std::make_unique<FakeSocket>(socket), state.path(), sweepIntervalMs);
    }
    ketplus::daemon::runner::remote::RemoteJobs& jobs() { return service->jobs(); }
};

} // namespace

class RemoteJobsServiceTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // Once started the service takes the runner messages of the socket; device messages are
    // not its. Stopped, it gives the socket back.
    void theServiceTakesTheSocketAndGivesItBack() {
        Service s;
        QVERIFY(!s.socket.take);
        s.service->start();
        QVERIFY(s.socket.take);
        QVERIFY(s.socket.reopened);
        QVERIFY(s.socket.deliver(jobChanged));
        QVERIFY(!s.socket.deliver({{"type", "device.revoked"}, {"deviceId", "device-1"}}));
        s.service.reset();
        QVERIFY(!s.socket.take);
        QVERIFY(!s.socket.reopened);
    }

    // Available only while the port offers runners and Git is usable.
    void availableFollowsThePort() {
        Service s;
        s.service->start();
        QCOMPARE(s.jobs().available(), prep::checkGit().isEmpty());
        s.port.available = false;
        QVERIFY(!s.jobs().available());
    }

    // The worktree is frozen on the thread pool, the answer comes back on the event loop and
    // then the Relay hears of the job through the port.
    void aSubmitGoesThroughGitOnThePoolToTheRelay() {
        if (!prep::checkGit().isEmpty())
            QSKIP("no usable system git");
        Service s;
        s.service->start();
        QTemporaryDir root;
        const QString cwd = repository(root.path());
        QVERIFY(!cwd.isEmpty());
        const QString id = s.f.remoteTask();
        QVERIFY(!id.isEmpty());
        QString code = QStringLiteral("unanswered");
        s.jobs().submit(id, requestIn(cwd), [&](const QString& answer) { code = answer; });
        QCOMPARE(code, QStringLiteral("unanswered")); // not inside the call
        QVERIFY(s.port.requests.isEmpty());
        QTRY_COMPARE(s.port.requests.size(), 1);
        const FakePort::Request submit = s.port.requests.takeFirst();
        QCOMPARE(submit.method, QByteArray("POST"));
        QCOMPARE(submit.path, QStringLiteral("/v2/runner-jobs"));
        QCOMPARE(submit.body.toObject().value("submitKey").toString(), id);
        submit.callback(RelayReply{202, queuedJob()});
        QCOMPARE(code, QString());
        const auto record = s.jobs().record(id);
        QVERIFY(record);
        QCOMPARE(record->placement, jobs::placement::relayQueued);
        QCOMPARE(record->jobId, QStringLiteral("job-1"));
        QVERIFY(record->snapshot);
        QVERIFY(QFileInfo(QDir(s.state.path()).filePath("runner-snapshots/stores")).isDir());
    }

    // The links are made for the current account only when this machine has an identity.
    void linksNeedTheMachineIdentity() {
        Service s;
        s.service->start();
        QVERIFY(s.socket.deliver(jobChanged));
        QVERIFY(!s.jobs().linksOpen());
        s.port.identity = machine();
        s.socket.reopened();
        QVERIFY(s.jobs().linksOpen());
        s.port.identity.reset();
        s.port.scope = QStringLiteral("account-2");
        s.socket.reopened();
        QVERIFY(!s.jobs().linksOpen());
    }

    // The sweep runs at start and then on its timer.
    void theSweepTimerClearsOldPacks() {
        QCOMPARE(RemoteJobsService::hourlySweepMs, 60 * 60 * 1000);
        Service s(20);
        s.service->start();
        const QString packs = QDir(s.state.path()).filePath("runner-snapshots/packs");
        QVERIFY(QDir().mkpath(packs));
        const QString stale = QDir(packs).filePath(QStringLiteral("stale.pack"));
        QFile file(stale);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("pack") == 4);
        QVERIFY(file.flush()); // before the time, which a later write would move
        QVERIFY(file.setFileTime(QDateTime::currentDateTimeUtc().addDays(-30),
                                 QFileDevice::FileModificationTime));
        file.close();
        QVERIFY(QFile::exists(stale)); // written after the sweep at start
        QTRY_VERIFY(!QFile::exists(stale));
    }

    // A service that goes while a snapshot is being made answers nothing and sends nothing.
    void aServiceThatGoesWhilePreparingAnswersNothing() {
        if (!prep::checkGit().isEmpty())
            QSKIP("no usable system git");
        Service s;
        s.service->start();
        QTemporaryDir root;
        const QString cwd = repository(root.path());
        QVERIFY(!cwd.isEmpty());
        const QString id = s.f.remoteTask();
        bool answered = false;
        s.jobs().submit(id, requestIn(cwd), [&](const QString&) { answered = true; });
        s.service.reset();
        QThreadPool::globalInstance()->waitForDone();
        QTest::qWait(100);
        QVERIFY(!answered);
        QVERIFY(s.port.requests.isEmpty());
    }
};

QTEST_GUILESS_MAIN(RemoteJobsServiceTest)
#include "RemoteJobsServiceTest.moc"
