#include "RunnerAppTest.h"

#include "cloud/CryptoPrimitives.h"
#include "commands/Commands.h"
#include "state/StateDirectory.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

namespace runner_app_test {
namespace {

QStringList messages;

void captureMessage(QtMsgType, const QMessageLogContext&, const QString& message) {
    messages.append(message);
}

// Text forms a key could take in output.
QStringList keyForms(const QByteArray& key) {
    return {QString::fromLatin1(ketplus::control_crypto::base64Url(key)),
            QString::fromLatin1(key.toBase64()), QString::fromLatin1(key.toHex())};
}

QByteArray runnerKey(const TestHost& host) {
    QFile file(host.stateDir() + "/runner.key");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Output, Qt log messages and (except the stored refresh credential) the state directory hold
// none of the secrets.
void verifyClean(TestHost& host, const QStringList& tokens, const QStringList& keys) {
    const QString output = host.output() + messages.join(QLatin1Char('\n'));
    const QByteArray state = host.stateBytes();
    const QString stored =
        QJsonDocument::fromJson([&] {
            QFile file(host.stateDir() + "/credential.json");
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        }())
            .object()
            .value("refreshCredential")
            .toString();
    for (const QString& secret : host.relay->secrets() + tokens + keys) {
        QVERIFY2(!output.contains(secret), "a secret reached the output or the log");
        if (secret != stored)
            QVERIFY2(!state.contains(secret.toUtf8()), "a secret reached the state directory");
    }
    for (const QString& token : tokens)
        QVERIFY(!state.contains(token.toUtf8()));
}

} // namespace

void RunnerAppRunTest::secretsNeverLeave() {
    messages.clear();
    const QtMessageHandler previous = qInstallMessageHandler(captureMessage);
    { // Every successful flow.
        TestHost host;
        host.relay->approveAfterPolls = 1;
        QStringList tokens{QString::fromLatin1(host.relay->createToken("desktop-1"))};
        host.stdinData = tokens.front().toUtf8();
        QCOMPARE(host.run({"register", "-", "--label", "gpu"}), 0);
        const QStringList keys = keyForms(runnerKey(host));
        tokens.append(
            QString::fromLatin1(host.relay->createToken("desktop-2", host.relay->lastRunnerId)));
        QCOMPARE(
            host.run({"pair", "--token-file", host.writeFile("token", tokens.last().toUtf8())}), 0);
        QCOMPARE(host.runLoop(3), 0);
        QCOMPARE(host.run({"status"}), 0);
        {
            QString error;
            const std::optional<ketplus::runner::app::StateDirectory> running =
                ketplus::runner::app::StateDirectory::open(host.stateDir(), &error);
            QVERIFY(running);
            QCOMPARE(host.run({"status"}), 0); // while `run` holds the lock
        }
        verifyClean(host, tokens, keys);
        QCOMPARE(host.run({"unpair", "desktop-2"}), 0);
        QCOMPARE(host.run({"deregister"}), 0);
        verifyClean(host, tokens, keys);
    }
    { // Failures: refusals, lost answers, mismatches and bad approvals.
        TestHost host;
        QStringList tokens{QString::fromLatin1(host.relay->createToken("desktop-1"))};
        host.relay->rewrite = [](const FakeRelay::Request& request, FakeRelay::Reply& reply) {
            reply.drop = request.path == "/v2/runners:register";
        };
        QCOMPARE(host.run({"register", tokens.last()}), 1);
        host.relay->rewrite = [](const FakeRelay::Request& request, FakeRelay::Reply& reply) {
            if (request.path == "/v2/runners:register") {
                QJsonObject desktop = reply.body.value("desktop").toObject();
                desktop.insert("machineKeyVersion", 3);
                reply.body.insert("desktop", desktop);
            }
        };
        tokens.append(QString::fromLatin1(host.relay->createToken("desktop-1")));
        QCOMPARE(host.run({"register", tokens.last()}), 4);
        host.relay->rewrite = nullptr;
        host.relay->approval = FakeRelay::Approval::BadSignature;
        tokens.append(QString::fromLatin1(host.relay->createToken("desktop-1")));
        QCOMPARE(host.run({"register", tokens.last()}), 5);
        const QStringList keys = keyForms(runnerKey(host));
        host.relay->intercept =
            [](const FakeRelay::Request& request) -> std::optional<FakeRelay::Reply> {
            if (request.path == "/v2/runners/credentials")
                return FakeRelay::Reply{401, {{"code", "invalid_access"}}, false};
            return std::nullopt;
        };
        QCOMPARE(host.run({"run", "--work-dir", host.workDir()}), 7);
        QCOMPARE(host.run({"deregister"}), 7);
        verifyClean(host, tokens, keys);
    }
    qInstallMessageHandler(previous);
}

void RunnerAppRunTest::helpListsEveryExitCode() {
    TestHost host;
    QCOMPARE(host.run({"--help"}), 0);
    for (int code = 0; code <= 9; ++code)
        QVERIFY2(host.output().contains(QStringLiteral("\n  %1  ").arg(code)),
                 qPrintable(QString::number(code)));
    QCOMPARE(host.run({"status", "--help"}), 0);
    QCOMPARE(host.run({"launch"}), 2);
    QCOMPARE(host.run({"status", "extra"}), 2);
    QCOMPARE(host.run({"register", "--label"}), 2);
    QCOMPARE(ketplus::runner::app::runCli({}, host.environment()), 2);
    QCOMPARE(host.run({"--version"}), 0);
    QVERIFY(host.output().contains("ketplus-runner 0.1.0-test"));
}

} // namespace runner_app_test
