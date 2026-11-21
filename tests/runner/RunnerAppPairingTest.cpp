#include "RunnerAppTest.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingIdentity.h"
#include "pairing/PairingPins.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

namespace pairing = ketplus::runner::pairing;

namespace runner_app_test {
namespace {

QJsonObject pinRecord(const TestHost& host, const QString& machineId) {
    QFile file(host.stateDir() + "/pins/" + machineId + ".json");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

// The code the runner should print, computed from the Relay's side of the pairing.
QString expectedCode(FakeRelay& relay, const QString& ticketId) {
    const FakeRelay::Ticket& ticket = relay.tickets[ticketId];
    const FakeRelay::Desktop& desktop = relay.desktop(ticket.machineId);
    const FakeRelay::Runner& runner = relay.runners[ticket.runnerId];
    return pairing::pairingCode(
               {ticket.id, relay.workspaceId, ticket.machineId, desktop.keyVersion,
                pairing::desktopFingerprint(desktop.signingPublicKey, desktop.agreementPublicKey),
                runner.id, 1, pairing::runnerFingerprint(runner.publicKey)})
        .code;
}

QString lastTicket(const FakeRelay& relay) {
    QString last;
    for (const FakeRelay::Ticket& ticket : relay.tickets) {
        if (last.isEmpty() || ticket.id.mid(7).toInt() > last.mid(7).toInt())
            last = ticket.id;
    }
    return last;
}

} // namespace

void RunnerAppPairingTest::registerPairsWithTheDesktop() {
    TestHost host;
    host.relay->approveAfterPolls = 2; // two "claimed" answers before the approval
    const QByteArray token = host.relay->createToken("desktop-1");
    QCOMPARE(host.run({"register", QString::fromLatin1(token), "--name", " node one ", "--label",
                       "gpu", "--label", "big"}),
             0);
    const FakeRelay::Request& request = host.relay->requests.front();
    QCOMPARE(request.path, "/v2/runners:register");
    QCOMPARE(request.body.value("name").toString(), "node one");
    QCOMPARE(request.body.value("os").toString(), "linux");
    QCOMPARE(request.body.value("arch").toString(), "x86_64");
    QCOMPARE(request.body.value("keyVersion").toInt(), 1);
    QCOMPARE(request.body.value("labels").toArray(), QJsonArray({"gpu", "big"}));
    QCOMPARE(request.body.value("capabilities").toArray(), QJsonArray({"runner.protocol/1"}));
    QCOMPARE(request.body.value("runnerVersion").toString(), "0.1.0-test");

    const QString ticketId = lastTicket(*host.relay);
    QVERIFY(host.output().contains("Confirmation code: " + expectedCode(*host.relay, ticketId)));
    QVERIFY(host.output().contains("Paired with desktop desktop-1."));
    QCOMPARE(host.relay->count("GET", "/v2/runner/pairing/tickets/"), 3);
    QCOMPARE(host.sleeps, (QList<qint64>{2000, 2000}));
    const QJsonObject pin = pinRecord(host, "desktop-1");
    QCOMPARE(pin.value("state").toString(), "active");
    QCOMPARE(pin.value("pairedAt").toString(), "2026-11-18T09:00:04.000Z");
    QVERIFY(!pin.contains("ticketId"));
}

void RunnerAppPairingTest::tokenFromStdinAndFile() {
    {
        TestHost host;
        host.stdinData = host.relay->createToken("desktop-1") + "\n";
        QCOMPARE(host.run({"register", "-"}), 0);
        QVERIFY(host.output().contains("Paired with desktop desktop-1."));
    }
    {
        TestHost host;
        const QString file = host.writeFile("token", host.relay->createToken("desktop-1") + "\n");
        QCOMPARE(host.run({"register", "--token-file", file}), 0);
        QVERIFY(host.output().contains("Paired with desktop desktop-1."));
        // Both sources at once is ambiguous.
        TestHost other;
        const QString second = other.writeFile("token", other.relay->createToken("desktop-1"));
        QCOMPARE(other.run({"register", "x", "--token-file", second}), 2);
    }
}

void RunnerAppPairingTest::refusesLocallyBeforeTheRelay_data() {
    QTest::addColumn<QStringList>("arguments");
    QTest::addColumn<bool>("registered");
    QTest::addColumn<int>("code");
    QTest::newRow("malformed token") << QStringList{"register", "kpr1.@@@"} << false << 2;
    QTest::newRow("not a token") << QStringList{"register", "hello"} << false << 2;
    QTest::newRow("token for an existing runner")
        << QStringList{"register", "ADD_DESKTOP"} << false << 2;
    QTest::newRow("label refused")
        << QStringList{"register", "NEW", "--label", "GPU"} << false << 2;
    QTest::newRow("duplicate label")
        << QStringList{"register", "NEW", "--label", "a", "--label", "a"} << false << 2;
    QTest::newRow("33 labels") << QStringList{"register", "NEW", "LABELS33"} << false << 2;
    QTest::newRow("name too long")
        << QStringList{"register", "NEW", "--name", QString(65, 'n')} << false << 2;
    QTest::newRow("name control character")
        << QStringList{"register", "NEW", "--name", "a\tb"} << false << 2;
    QTest::newRow("blank name") << QStringList{"register", "NEW", "--name", "  "} << false << 2;
    QTest::newRow("already registered") << QStringList{"register", "NEW"} << true << 2;
    QTest::newRow("pair without registering") << QStringList{"pair", "ADD_DESKTOP"} << false << 2;
    QTest::newRow("pair with a new-runner token") << QStringList{"pair", "NEW"} << true << 2;
    QTest::newRow("unknown option") << QStringList{"register", "NEW", "--force"} << false << 2;
}

void RunnerAppPairingTest::refusesLocallyBeforeTheRelay() {
    QFETCH(QStringList, arguments);
    QFETCH(bool, registered);
    QFETCH(int, code);
    TestHost host;
    const QString runnerId = registered ? host.registerRunner() : QStringLiteral("runner-x");
    const QByteArray before = host.stateBytes();
    host.relay->requests.clear();
    for (QString& argument : arguments) {
        if (argument == "NEW")
            argument = QString::fromLatin1(host.relay->createToken("desktop-2"));
        else if (argument == "ADD_DESKTOP")
            argument = QString::fromLatin1(host.relay->createToken("desktop-2", runnerId));
    }
    if (arguments.removeOne("LABELS33")) {
        for (int i = 0; i < 33; ++i)
            arguments << "--label" << QString::number(i);
    }
    QCOMPARE(host.run(arguments), code);
    QVERIFY(host.relay->requests.isEmpty());
    QCOMPARE(host.stateBytes(), before);
}

void RunnerAppPairingTest::pairAddsASecondDesktop() {
    TestHost host;
    const QString runnerId = host.registerRunner();
    const QByteArray token = host.relay->createToken("desktop-2", runnerId);
    QCOMPARE(host.run({"pair", QString::fromLatin1(token)}), 0);
    QVERIFY(host.output().contains("Confirmation code: " +
                                   expectedCode(*host.relay, lastTicket(*host.relay))));
    QCOMPARE(pinRecord(host, "desktop-1").value("state").toString(), "active");
    QCOMPARE(pinRecord(host, "desktop-2").value("state").toString(), "active");
    QCOMPARE(host.relay->count("POST", "/claim"), 1);
}

void RunnerAppPairingTest::pairRefusesATokenForAnotherRunner() {
    TestHost host;
    host.registerRunner();
    host.relay->requests.clear();
    QCOMPARE(host.run({"pair", QString::fromLatin1(host.relay->createToken(
                                   "desktop-2", QStringLiteral("runner-x")))}),
             2);
    host.relay->workspaceId = "ws-other";
    const QString runnerId = host.relay->lastRunnerId;
    QCOMPARE(
        host.run({"pair", QString::fromLatin1(host.relay->createToken("desktop-2", runnerId))}), 2);
    QVERIFY(host.relay->requests.isEmpty());
}

void RunnerAppPairingTest::desktopKeyMismatchOnRegister_data() {
    QTest::addColumn<QString>("field");
    QTest::newRow("signing key") << "signingPublicKey";
    QTest::newRow("agreement key") << "agreementPublicKey";
    QTest::newRow("machine id") << "machineId";
    QTest::newRow("key version") << "machineKeyVersion";
}

void RunnerAppPairingTest::desktopKeyMismatchOnRegister() {
    QFETCH(QString, field);
    TestHost host;
    const QString other = QString::fromLatin1(
        ketplus::control_crypto::base64Url(ketplus::control_crypto::generateEd25519().publicKey));
    host.relay->rewrite = [&](const FakeRelay::Request& request, FakeRelay::Reply& reply) {
        if (request.path != "/v2/runners:register")
            return;
        QJsonObject desktop = reply.body.value("desktop").toObject();
        desktop.insert(field, field == "machineKeyVersion" ? QJsonValue(2)
                              : field == "machineId"       ? QJsonValue("desktop-9")
                                                           : QJsonValue(other));
        reply.body.insert("desktop", desktop);
    };
    QCOMPARE(host.run({"register", QString::fromLatin1(host.relay->createToken("desktop-1"))}), 4);
    QVERIFY(host.output().contains("desktop_key_mismatch"));
    QCOMPARE(host.relay->count("POST", "/v2/runner:deregister"), 1);
    QVERIFY(host.relay->runners.first().removed);
    QCOMPARE(host.stateFiles(), QStringList{host.stateDir() + "/lock"});
    QVERIFY(!host.output().contains("Confirmation code"));
}

void RunnerAppPairingTest::desktopKeyMismatchOnPair() {
    TestHost host;
    const QString runnerId = host.registerRunner();
    host.relay->rewrite = [](const FakeRelay::Request& request, FakeRelay::Reply& reply) {
        if (request.path.endsWith("/claim")) {
            QJsonObject desktop = reply.body.value("desktop").toObject();
            desktop.insert("machineKeyVersion", 7);
            reply.body.insert("desktop", desktop);
        }
    };
    QCOMPARE(
        host.run({"pair", QString::fromLatin1(host.relay->createToken("desktop-2", runnerId))}), 4);
    QCOMPARE(host.relay->count("POST", "/v2/runner/pairs/desktop-2/revoke"), 1);
    QVERIFY(!QFile::exists(host.stateDir() + "/pins/desktop-2.json"));
    QCOMPARE(pinRecord(host, "desktop-1").value("state").toString(), "active");
}

void RunnerAppPairingTest::approvalThatDoesNotVerify_data() {
    QTest::addColumn<int>("approval");
    QTest::newRow("signed by another key") << int(FakeRelay::Approval::BadSignature);
    QTest::newRow("signed over another digest") << int(FakeRelay::Approval::WrongDigest);
}

void RunnerAppPairingTest::approvalThatDoesNotVerify() {
    QFETCH(int, approval);
    TestHost host;
    host.relay->approval = FakeRelay::Approval(approval);
    QCOMPARE(host.run({"register", QString::fromLatin1(host.relay->createToken("desktop-1"))}), 5);
    QVERIFY(!QFile::exists(host.stateDir() + "/pins/desktop-1.json"));
    QCOMPARE(host.relay->count("POST", "/v2/runner/pairs/desktop-1/revoke"), 1);
    QVERIFY(host.output().contains("does not verify"));
}

void RunnerAppPairingTest::pairingThatEnds_data() {
    QTest::addColumn<int>("approval");
    QTest::addColumn<QString>("reason");
    QTest::newRow("rejected") << int(FakeRelay::Approval::Reject) << "rejected";
    QTest::newRow("expired") << int(FakeRelay::Approval::Expire) << "expired";
    QTest::newRow("revoked") << int(FakeRelay::Approval::Revoke) << "revoked";
    QTest::newRow("unknown ticket") << int(FakeRelay::Approval::Forget) << "ticket_not_found";
}

void RunnerAppPairingTest::pairingThatEnds() {
    QFETCH(int, approval);
    QFETCH(QString, reason);
    TestHost host;
    host.relay->approval = FakeRelay::Approval(approval);
    QCOMPARE(host.run({"register", QString::fromLatin1(host.relay->createToken("desktop-1"))}), 6);
    QVERIFY(!QFile::exists(host.stateDir() + "/pins/desktop-1.json"));
    QVERIFY2(host.output().contains("ended (" + reason + ")"), qPrintable(host.output()));
    // The runner itself stays registered for a new token.
    QVERIFY(QFile::exists(host.stateDir() + "/credential.json"));
}

void RunnerAppPairingTest::lostRegisterAnswer() {
    { // Lost twice: nothing is kept, and the user learns what to do.
        TestHost host;
        host.relay->rewrite = [](const FakeRelay::Request& request, FakeRelay::Reply& reply) {
            reply.drop = request.path == "/v2/runners:register";
        };
        QCOMPARE(host.run({"register", QString::fromLatin1(host.relay->createToken("desktop-1"))}),
                 1);
        QCOMPARE(host.relay->count("POST", "/v2/runners:register"), 2);
        const QList<FakeRelay::Request>& requests = host.relay->requests;
        QCOMPARE(requests.at(0).raw, requests.at(1).raw); // same issuedAt and signature
        QCOMPARE(host.stateFiles(), QStringList{host.stateDir() + "/lock"});
        QVERIFY(host.output().contains("reject the waiting pairing request"));
        QVERIFY(!host.output().contains("Confirmation code"));
    }
    { // Lost once, then the retry finds the ticket spent.
        TestHost host;
        host.relay->rewrite = [n = 0](const FakeRelay::Request& request,
                                      FakeRelay::Reply& reply) mutable {
            reply.drop = request.path == "/v2/runners:register" && n++ == 0;
        };
        QCOMPARE(host.run({"register", QString::fromLatin1(host.relay->createToken("desktop-1"))}),
                 1);
        QVERIFY(host.output().contains("create a new pairing token"));
        QCOMPARE(host.stateFiles(), QStringList{host.stateDir() + "/lock"});
    }
    { // Lost once before the Relay saw it: the retry succeeds.
        TestHost host;
        host.relay->intercept =
            [n = 0](const FakeRelay::Request& request) mutable -> std::optional<FakeRelay::Reply> {
            if (request.path == "/v2/runners:register" && n++ == 0)
                return FakeRelay::Reply{0, {}, true};
            return std::nullopt;
        };
        QCOMPARE(host.run({"register", QString::fromLatin1(host.relay->createToken("desktop-1"))}),
                 0);
    }
}

void RunnerAppPairingTest::lostClaimAnswer() {
    TestHost host;
    const QString runnerId = host.registerRunner();
    host.relay->rewrite = [](const FakeRelay::Request& request, FakeRelay::Reply& reply) {
        reply.drop = request.path.endsWith("/claim");
    };
    QCOMPARE(
        host.run({"pair", QString::fromLatin1(host.relay->createToken("desktop-2", runnerId))}), 1);
    QCOMPARE(host.relay->count("POST", "/claim"), 2);
    QVERIFY(!QFile::exists(host.stateDir() + "/pins/desktop-2.json"));
    QVERIFY(host.output().contains("reject the waiting pairing request"));
}

void RunnerAppPairingTest::serverErrorIsALostAnswer() {
    const auto gateway = [](const int status) {
        FakeRelay::Reply reply;
        reply.status = status;
        reply.raw = "<html><body>Bad Gateway</body></html>";
        return reply;
    };
    { // A 502 the first time: the same body is sent once more and goes through.
        TestHost host;
        host.relay->intercept =
            [&,
             n = 0](const FakeRelay::Request& request) mutable -> std::optional<FakeRelay::Reply> {
            if (request.path == "/v2/runners:register" && n++ == 0)
                return gateway(502);
            return std::nullopt;
        };
        QCOMPARE(host.run({"register", QString::fromLatin1(host.relay->createToken("desktop-1"))}),
                 0);
        QCOMPARE(host.relay->count("POST", "/v2/runners:register"), 2);
        QCOMPARE(host.relay->requests.at(0).raw, host.relay->requests.at(1).raw);
    }
    { // 502 twice, the second with a contract error body: lost, nothing kept, no code shown.
        TestHost host;
        host.relay->intercept =
            [&,
             n = 0](const FakeRelay::Request& request) mutable -> std::optional<FakeRelay::Reply> {
            if (request.path != "/v2/runners:register")
                return std::nullopt;
            return n++ == 0 ? gateway(502) : FakeRelay::Reply{502, {{"code", "internal"}}, false};
        };
        QCOMPARE(host.run({"register", QString::fromLatin1(host.relay->createToken("desktop-1"))}),
                 1);
        QCOMPARE(host.relay->count("POST", "/v2/runners:register"), 2);
        QCOMPARE(host.stateFiles(), QStringList{host.stateDir() + "/lock"});
        QVERIFY(host.output().contains("reject the waiting pairing request"));
        QVERIFY(host.output().contains("create a new pairing token"));
        QVERIFY(!host.output().contains("Confirmation code"));
    }
    { // A claim answered 502 twice: no pin, the same advice.
        TestHost host;
        const QString runnerId = host.registerRunner();
        host.relay->requests.clear();
        host.out.clear();
        host.err.clear();
        host.relay->intercept =
            [&](const FakeRelay::Request& request) -> std::optional<FakeRelay::Reply> {
            if (request.path.endsWith("/claim"))
                return gateway(502);
            return std::nullopt;
        };
        QCOMPARE(
            host.run({"pair", QString::fromLatin1(host.relay->createToken("desktop-2", runnerId))}),
            1);
        QCOMPARE(host.relay->count("POST", "/claim"), 2);
        QVERIFY(!QFile::exists(host.stateDir() + "/pins/desktop-2.json"));
        QVERIFY(host.output().contains("reject the waiting pairing request"));
        QVERIFY(!host.output().contains("Confirmation code"));
    }
}

void RunnerAppPairingTest::unreadableClientErrorIsARefusal() {
    TestHost host;
    host.relay->intercept =
        [](const FakeRelay::Request& request) -> std::optional<FakeRelay::Reply> {
        if (request.path != "/v2/runners:register")
            return std::nullopt;
        FakeRelay::Reply reply;
        reply.status = 400;
        reply.raw = "<html>Bad Request</html>";
        return reply;
    };
    QCOMPARE(host.run({"register", QString::fromLatin1(host.relay->createToken("desktop-1"))}), 1);
    QCOMPARE(host.relay->count("POST", "/v2/runners:register"), 1); // a refusal is not retried
    QVERIFY(host.output().contains("unknown"));
    QVERIFY(!host.output().contains("reject the waiting pairing request"));
    QCOMPARE(host.stateFiles(), QStringList{host.stateDir() + "/lock"});
}

void RunnerAppPairingTest::unsupportedPlatform() {
    for (const auto& [os, arch] : {std::pair{"darwin", "arm64"}, std::pair{"linux", "i386"},
                                   std::pair{"linux", "riscv64"}}) {
        TestHost host;
        host.platform = {os, arch};
        QCOMPARE(host.run({"register", QString::fromLatin1(host.relay->createToken("desktop-1"))}),
                 9);
        QCOMPARE(host.run({"pair", "x"}), 9);
        QCOMPARE(host.run({"run"}), 9);
        QVERIFY(host.relay->requests.isEmpty());
        QVERIFY(!QFile::exists(host.stateDir() + "/runner.key"));
    }
}

} // namespace runner_app_test
