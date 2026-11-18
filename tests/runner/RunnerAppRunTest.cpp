#include "RunnerAppTest.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingIdentity.h"
#include "pairing/PairingSignatures.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

#include <openssl/evp.h>

namespace pairing = ketplus::runner::pairing;
namespace crypto = ketplus::control_crypto;

namespace runner_app_test {
namespace {

QJsonObject record(const TestHost& host, const QString& name) {
    QFile file(host.stateDir() + "/" + name);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()
                                          : QJsonObject();
}

QString storedCredential(const TestHost& host) {
    return record(host, "credential.json").value("refreshCredential").toString();
}

// Starts a run on `host` that stops after `sleeps` waits.
int runFor(TestHost& host, const int sleeps) { return host.runLoop(sleeps); }

FakeRelay::Reply refusal(const int status, const QString& code) {
    return {status, {{"code", code}, {"message", code}}, false};
}

} // namespace

void RunnerAppRunTest::heartbeatEveryThirtySeconds() {
    TestHost host;
    host.registerRunner({"--label", "gpu"});
    host.relay->requests.clear();
    QCOMPARE(runFor(host, 3), 0);
    QCOMPARE(host.relay->count("POST", "/v2/runner/heartbeat"), 3);
    QCOMPARE(host.sleeps, (QList<qint64>{30000, 30000, 30000}));
    for (const FakeRelay::Request& request : host.relay->requests) {
        if (request.path != "/v2/runner/heartbeat")
            continue;
        QCOMPARE(request.body.keys(),
                 (QStringList{"capabilities", "currentJob", "labels", "runnerVersion", "state"}));
        // The production wiring takes no job yet: its command runner is never ready.
        QCOMPARE(request.body.value("state").toString(), "draining");
        QCOMPARE(request.body.value("labels").toArray(), QJsonArray{"gpu"});
        QCOMPARE(request.body.value("capabilities").toArray(), QJsonArray{"runner.protocol/1"});
        QCOMPARE(request.body.value("runnerVersion").toString(), "0.1.0-test");
        QVERIFY(request.body.value("currentJob").isNull());
    }
    QCOMPARE(host.relay->count("POST", "/v2/runners/challenges"), 1);
}

void RunnerAppRunTest::tokenSignsTheChallengeAndRotatesTheCredential() {
    TestHost host;
    const QString runnerId = host.registerRunner();
    // register rotated the credential once and stored the new one.
    QCOMPARE(storedCredential(host), host.relay->runners[runnerId].current);
    host.relay->requests.clear();
    QCOMPARE(runFor(host, 1), 0);

    const FakeRelay::Request& challenge = host.relay->requests.at(0);
    const FakeRelay::Request& credential = host.relay->requests.at(1);
    QCOMPARE(challenge.path, "/v2/runners/challenges");
    QCOMPARE(credential.path, "/v2/runners/credentials");
    const QJsonObject& body = credential.body;
    pairing::ChallengePayload payload{runnerId, 1, body.value("nonce").toString(),
                                      body.value("issuedAt").toString()};
    QFile key(host.stateDir() + "/runner.key");
    QVERIFY(key.open(QIODevice::ReadOnly));
    const QByteArray publicKey = crypto::publicKey(key.readAll(), EVP_PKEY_ED25519);
    QVERIFY(crypto::verifyEd25519(
        publicKey, payload.bytes(),
        pairing::decodeBase64Url(body.value("signature").toString()).value()));
    QCOMPARE(QString::fromUtf8(payload.bytes()).left(30), "[\"ketplus\",2,\"runner-challenge");
    QCOMPARE(storedCredential(host), host.relay->runners[runnerId].current);
    for (const QString& token : host.relay->accessTokens)
        QVERIFY(!host.stateBytes().contains(token.toUtf8()));
}

void RunnerAppRunTest::lostTokenAnswerKeepsTheRunnerWorking() {
    TestHost host;
    const QString runnerId = host.registerRunner();
    const QString before = storedCredential(host);
    host.relay->requests.clear();
    host.relay->rewrite = [n = 0](const FakeRelay::Request& request,
                                  FakeRelay::Reply& reply) mutable {
        reply.drop = request.path == "/v2/runners/credentials" && n++ == 0;
    };
    QCOMPARE(runFor(host, 2), 0);
    QCOMPARE(host.sleeps.front(), 1000); // backoff, then a new challenge with the old credential
    QCOMPARE(host.relay->count("POST", "/v2/runners/credentials"), 2);
    QCOMPARE(host.relay->requests.at(1).body.value("refreshCredential").toString(), before);
    QCOMPARE(host.relay->requests.at(3).body.value("refreshCredential").toString(), before);
    QCOMPARE(storedCredential(host), host.relay->runners[runnerId].current);
    QCOMPARE(host.relay->count("POST", "/v2/runner/heartbeat"), 1);
}

void RunnerAppRunTest::stopBetweenRotationAndStore() {
    TestHost host;
    const QString runnerId = host.registerRunner();
    const QString before = storedCredential(host);
    // The Relay rotates, the answer is lost and the process stops before storing anything.
    host.relay->rewrite = [](const FakeRelay::Request& request, FakeRelay::Reply& reply) {
        reply.drop = request.path == "/v2/runners/credentials";
    };
    QCOMPARE(runFor(host, 1), 0);
    QCOMPARE(storedCredential(host), before);
    QVERIFY(host.relay->runners[runnerId].current != before);
    // The next process still gets a token.
    host.relay->rewrite = nullptr;
    QCOMPARE(runFor(host, 1), 0);
    QCOMPARE(host.relay->count("POST", "/v2/runner/heartbeat"), 1);
    // Stopped after storing: the next process uses the stored credential.
    QCOMPARE(runFor(host, 1), 0);
    QCOMPARE(host.relay->count("POST", "/v2/runner/heartbeat"), 2);
    // A credential two rotations old is refused for good; the state stays as it is.
    QJsonObject credential = record(host, "credential.json");
    credential.insert("refreshCredential", before);
    QFile file(host.stateDir() + "/credential.json");
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(credential).toJson());
    file.close();
    const QByteArray state = host.stateBytes();
    QCOMPARE(runFor(host, 1), 7);
    QCOMPARE(host.stateBytes(), state);
    QVERIFY(host.output().contains("Register it again"));
}

void RunnerAppRunTest::tokenRenewsBeforeItExpires() {
    TestHost host;
    host.registerRunner();
    host.relay->requests.clear();
    QCOMPARE(runFor(host, 30), 0); // 15 minutes of heartbeats
    QCOMPARE(host.relay->count("POST", "/v2/runners/challenges"), 2);
    for (const FakeRelay::Request& request : host.relay->requests)
        QVERIFY(!request.authorization.isEmpty() || request.path.startsWith("/v2/runners/"));
}

void RunnerAppRunTest::fatalRefusalsStopTheRun_data() {
    QTest::addColumn<QString>("path");
    QTest::addColumn<int>("status");
    QTest::addColumn<QString>("code");
    QTest::addColumn<int>("exit");
    QTest::newRow("token invalid_access")
        << "/v2/runners/credentials" << 401 << "invalid_access" << 7;
    QTest::newRow("token runner_removed")
        << "/v2/runners/credentials" << 403 << "runner_removed" << 8;
    QTest::newRow("challenge runner_removed")
        << "/v2/runners/challenges" << 403 << "runner_removed" << 8;
    QTest::newRow("challenge runner_not_found")
        << "/v2/runners/challenges" << 404 << "runner_not_found" << 8;
    QTest::newRow("heartbeat runner_removed")
        << "/v2/runner/heartbeat" << 403 << "runner_removed" << 8;
}

void RunnerAppRunTest::fatalRefusalsStopTheRun() {
    QFETCH(QString, path);
    QFETCH(int, status);
    QFETCH(QString, code);
    QFETCH(int, exit);
    TestHost host;
    host.registerRunner();
    host.relay->intercept =
        [&](const FakeRelay::Request& request) -> std::optional<FakeRelay::Reply> {
        if (request.path == path)
            return refusal(status, code);
        return std::nullopt;
    };
    const QStringList files = host.stateFiles();
    QCOMPARE(runFor(host, 5), exit);
    QVERIFY(host.sleeps.isEmpty());
    for (const QString& file : files) // nothing deleted
        QVERIFY2(host.stateFiles().contains(file), qPrintable(file));
}

void RunnerAppRunTest::retriedRefusalsBackOff_data() {
    QTest::addColumn<QString>("path");
    QTest::addColumn<int>("status");
    QTest::addColumn<QString>("code");
    for (const char* path : {"/v2/runners/credentials", "/v2/runner/heartbeat"}) {
        for (const auto& [status, code] :
             {std::pair{403, "runner_disabled"}, std::pair{403, "entitlement_required"},
              std::pair{403, "unsupported"}, std::pair{429, "rate_limited"},
              std::pair{503, "server_error"}, std::pair{0, "network"}}) {
            QTest::addRow("%s %s", path, code) << path << status << code;
        }
    }
}

void RunnerAppRunTest::retriedRefusalsBackOff() {
    QFETCH(QString, path);
    QFETCH(int, status);
    QFETCH(QString, code);
    TestHost host;
    host.registerRunner();
    host.relay->intercept =
        [&, n = 0](const FakeRelay::Request& request) mutable -> std::optional<FakeRelay::Reply> {
        if (request.path != path || n++ >= 8)
            return std::nullopt;
        return status == 0 ? FakeRelay::Reply{0, {}, true} : refusal(status, code);
    };
    QCOMPARE(runFor(host, 9), 0);
    QCOMPARE(host.sleeps,
             (QList<qint64>{1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 30000}));
    QCOMPARE(host.relay->count("POST", "/v2/runner/heartbeat") >= 1, true);

    // With the largest jitter the delay still never passes 60 seconds.
    TestHost jittered;
    jittered.registerRunner();
    jittered.randomValue = 0.999;
    jittered.relay->intercept =
        [&](const FakeRelay::Request& request) -> std::optional<FakeRelay::Reply> {
        if (request.path == path)
            return status == 0 ? FakeRelay::Reply{0, {}, true} : refusal(status, code);
        return std::nullopt;
    };
    QCOMPARE(runFor(jittered, 10), 0);
    QCOMPARE(jittered.sleeps.at(1), 2999);
    for (const qint64 delay : jittered.sleeps)
        QVERIFY(delay <= 60000);
    QCOMPARE(jittered.sleeps.last(), 60000);
}

void RunnerAppRunTest::heartbeatInvalidAccessRenewsOnce() {
    TestHost host;
    host.registerRunner();
    host.relay->requests.clear();
    host.relay->intercept =
        [n = 0](const FakeRelay::Request& request) mutable -> std::optional<FakeRelay::Reply> {
        if (request.path == "/v2/runner/heartbeat" && n++ == 0)
            return refusal(401, "invalid_access");
        return std::nullopt;
    };
    QCOMPARE(runFor(host, 1), 0);
    QCOMPARE(host.relay->count("POST", "/v2/runners/challenges"), 2);
    QCOMPARE(host.relay->count("POST", "/v2/runner/heartbeat"), 2);
    QCOMPARE(host.sleeps, QList<qint64>{30000});
}

void RunnerAppRunTest::disabledRunnerKeepsRunning() {
    TestHost host;
    host.registerRunner();
    host.relay->disabled = true;
    QCOMPARE(runFor(host, 3), 0);
    QCOMPARE(host.relay->count("POST", "/v2/runner/heartbeat"), 3);
    QVERIFY(host.output().contains("disabled"));
}

void RunnerAppRunTest::runFinishesPendingPairings() {
    TestHost host;
    host.relay->approval = FakeRelay::Approval::Never;
    host.stopAfterSleeps = 2;
    QCOMPARE(host.run({"register", QString::fromLatin1(host.relay->createToken("desktop-1"))}), 1);
    QCOMPARE(record(host, "pins/desktop-1.json").value("state").toString(), "pending");
    QVERIFY(host.output().contains("stays pending"));

    host.relay->approval = FakeRelay::Approval::Approve;
    QCOMPARE(runFor(host, 1), 0);
    QCOMPARE(record(host, "pins/desktop-1.json").value("state").toString(), "active");
    QVERIFY(host.output().contains("Paired with desktop desktop-1."));

    // A pending pin whose approval does not verify is dropped; the run goes on.
    host.relay->approval = FakeRelay::Approval::Never;
    host.stop = false;
    host.stopAfterSleeps = 1;
    host.sleeps.clear();
    QCOMPARE(host.run({"pair", QString::fromLatin1(host.relay->createToken(
                                   "desktop-2", host.relay->lastRunnerId))}),
             1);
    host.relay->approval = FakeRelay::Approval::BadSignature;
    QCOMPARE(runFor(host, 2), 0);
    QVERIFY(!QFile::exists(host.stateDir() + "/pins/desktop-2.json"));
    QCOMPARE(host.relay->count("POST", "/v2/runner/pairs/desktop-2/revoke"), 1);
    QCOMPARE(host.relay->count("POST", "/v2/runner/heartbeat"), 3);
}

// The command line with the real event loop and timers; each run ends well within 2 seconds.
void RunnerAppRunTest::runCliExitsWhenTheRunnerIsRemoved() {
    TestHost host;
    host.registerRunner();
    host.relay->intercept =
        [](const FakeRelay::Request& request) -> std::optional<FakeRelay::Reply> {
        if (request.path == "/v2/runner/heartbeat")
            return refusal(403, "runner_removed");
        return std::nullopt;
    };
    QElapsedTimer elapsed;
    elapsed.start();
    QCOMPARE(host.run({"run", "--work-dir", host.workDir()}), 8);
    QVERIFY(elapsed.elapsed() < 2000);
    QCOMPARE(host.relay->count("POST", "/v2/runner/heartbeat"), 1);
}

void RunnerAppRunTest::runCliStopsWhenAsked() {
    TestHost host;
    host.registerRunner();
    host.stop = true;
    QElapsedTimer elapsed;
    elapsed.start();
    QCOMPARE(host.run({"run", "--work-dir", host.workDir()}), 0);
    QVERIFY(elapsed.elapsed() < 2000);
    QVERIFY(host.output().contains("Stopped."));
    QVERIFY(QFileInfo(host.workDir()).isDir());
}

void RunnerAppRunTest::unpairRemovesThePinFirst() {
    TestHost host;
    host.registerRunner();
    host.relay->rewrite = [](const FakeRelay::Request& request, FakeRelay::Reply& reply) {
        reply.drop = request.path.endsWith("/revoke");
    };
    QCOMPARE(host.run({"unpair", "desktop-1"}), 1);
    QVERIFY(!QFile::exists(host.stateDir() + "/pins/desktop-1.json"));
    QVERIFY(host.output().contains("run `ketplus-runner unpair desktop-1` again"));
    host.relay->rewrite = nullptr;
    // The Relay already ended the pair; not_paired counts as done.
    QCOMPARE(host.run({"unpair", "desktop-1"}), 0);
    QCOMPARE(host.relay->count("POST", "/v2/runner/pairs/desktop-1/revoke"), 2);
    QCOMPARE(host.run({"unpair", "../x"}), 2);
}

void RunnerAppRunTest::deregisterRemovesEverything() {
    TestHost host;
    const QString runnerId = host.registerRunner();
    host.relay->intercept =
        [](const FakeRelay::Request& request) -> std::optional<FakeRelay::Reply> {
        if (request.path == "/v2/runner:deregister")
            return refusal(429, "rate_limited");
        return std::nullopt;
    };
    const QStringList files = host.stateFiles();
    QCOMPARE(host.run({"deregister"}), 1);
    QCOMPARE(host.stateFiles(), files);

    host.relay->intercept = nullptr;
    QCOMPARE(host.run({"deregister"}), 0);
    QCOMPARE(host.stateFiles(), QStringList{host.stateDir() + "/lock"});
    QVERIFY(host.relay->runners[runnerId].removed);
    QCOMPARE(host.run({"status"}), 0);
    QVERIFY(host.output().contains("Not registered."));

    // Refused for good: nothing is removed, and the message names --local-only.
    TestHost refused;
    refused.registerRunner();
    refused.relay->intercept =
        [](const FakeRelay::Request& request) -> std::optional<FakeRelay::Reply> {
        if (request.path == "/v2/runners/credentials")
            return refusal(401, "invalid_access");
        return std::nullopt;
    };
    const QByteArray state = refused.stateBytes();
    QCOMPARE(refused.run({"deregister"}), 7);
    QCOMPARE(refused.stateBytes(), state);
    QVERIFY(refused.output().contains("--local-only"));
}

void RunnerAppRunTest::deregisterLocalOnly() {
    TestHost host;
    host.registerRunner();
    host.relay->requests.clear();
    QCOMPARE(host.run({"deregister", "--local-only"}), 0);
    QVERIFY(host.relay->requests.isEmpty());
    QCOMPARE(host.stateFiles(), QStringList{host.stateDir() + "/lock"});
    QVERIFY(host.output().contains("Remove the runner on each desktop"));
}

void RunnerAppRunTest::statusShowsNoSecret() {
    TestHost host;
    const QString runnerId = host.registerRunner({"--name", "node one", "--label", "gpu"});
    host.relay->approval = FakeRelay::Approval::Never;
    host.stopAfterSleeps = host.sleeps.size() + 1;
    QCOMPARE(
        host.run({"pair", QString::fromLatin1(host.relay->createToken("desktop-2", runnerId))}), 1);
    host.out.clear();
    host.err.clear();
    QCOMPARE(host.run({"status"}), 0);
    const QString output = host.output();
    QVERIFY(output.contains("Runner: " + runnerId));
    QVERIFY(output.contains("Workspace: ws-1"));
    QVERIFY(output.contains("Name: node one"));
    QVERIFY(output.contains("Labels: gpu"));
    QVERIFY(output.contains("Desktop desktop-1: paired since 2026-11-18T09:00:00.000Z"));
    QVERIFY(output.contains("Desktop desktop-2: waiting for approval"));
    for (const QString& secret : host.relay->secrets())
        QVERIFY(!output.contains(secret));
}

} // namespace runner_app_test
