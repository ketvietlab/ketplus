#include "cloud/MachineChallenge.h"
#include "cloud-privacy/CloudPrivacyTestAccess.h"
#include "cloud-privacy/FakeTransport.h"
#include "cloud-privacy/MemoryCredentials.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <openssl/evp.h>

#include <limits>
#include <memory>
#include <optional>

using namespace ketplus::daemon;
using Access = CloudPrivacyTestAccess;

namespace {
QJsonObject challenge() {
    return {{"id", "challenge_1"},
            {"machineId", "machine-synthetic"},
            {"keyVersion", 1},
            {"signatureVersion", 2},
            {"signatureAlgorithm", "Ed25519"},
            {"challenge", QString::fromLatin1(CloudCrypto::base64Url(QByteArray(32, 'k')))}};
}

bool verify(const QByteArray& key, const QByteArray& bytes, const QByteArray& signature) {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> publicKey(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                    reinterpret_cast<const unsigned char*>(key.constData()),
                                    static_cast<size_t>(key.size())),
        EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    return publicKey && context &&
           EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, publicKey.get()) == 1 &&
           EVP_DigestVerify(context.get(),
                            reinterpret_cast<const unsigned char*>(signature.constData()),
                            static_cast<size_t>(signature.size()),
                            reinterpret_cast<const unsigned char*>(bytes.constData()),
                            static_cast<size_t>(bytes.size())) == 1;
}
} // namespace

class MachineChallengeTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { r03::credentials.clear(); }

    void matchesRelayVectorAndRejectsCrossDomainSignature() {
        QFile file(QStringLiteral(KETPLUS_CHALLENGE_FIXTURE));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto vector = QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!vector.isEmpty());
        const auto bytes = machine_challenge::signingBytes(
            vector, vector.value("machineId").toString(), vector.value("keyVersion").toInt());
        QCOMPARE(bytes.toHex(), vector.value("signingBytesHex").toString().toLatin1());
        const auto key = QByteArray::fromHex(vector.value("publicKeyHex").toString().toLatin1());
        const auto signature =
            QByteArray::fromHex(vector.value("signatureHex").toString().toLatin1());
        QVERIFY(verify(key, bytes, signature));
        QVERIFY(!verify(key, CloudCrypto::fromBase64Url(vector.value("challenge").toString()),
                        signature));
        auto wrongDomain = bytes;
        wrongDomain.replace("daemon-challenge", "webrtc-answer");
        QVERIFY(!verify(key, wrongDomain, signature));
    }

    void rejectsMalformedOrUnboundResponse_data() {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        for (const char* field : {"id", "machineId", "keyVersion", "signatureVersion",
                                  "signatureAlgorithm", "challenge"}) {
            QTest::newRow(qPrintable(QString(field) + "-missing"))
                << QString(field) << QJsonValue(QJsonValue::Undefined);
            QTest::newRow(qPrintable(QString(field) + "-null"))
                << QString(field) << QJsonValue(QJsonValue::Null);
        }
        QTest::newRow("version-string") << QString("signatureVersion") << QJsonValue("2");
        QTest::newRow("version-fraction") << QString("signatureVersion") << QJsonValue(2.1);
        QTest::newRow("version-legacy") << QString("signatureVersion") << QJsonValue(1);
        QTest::newRow("version-unknown") << QString("signatureVersion") << QJsonValue(3);
        QTest::newRow("algorithm") << QString("signatureAlgorithm") << QJsonValue("ed25519");
        QTest::newRow("key-string") << QString("keyVersion") << QJsonValue("1");
        QTest::newRow("key-fraction") << QString("keyVersion") << QJsonValue(1.1);
        QTest::newRow("key-other") << QString("keyVersion") << QJsonValue(2);
        QTest::newRow("key-overflow") << QString("keyVersion") << QJsonValue(2147483648.0);
        QTest::newRow("machine-other") << QString("machineId") << QJsonValue("other-machine");
        for (const auto& id : {QString(), QString(129, 'a'), QString("id\n"), QString("id/1"),
                               QString::fromUtf8("id-é")})
            QTest::newRow(qPrintable("id-invalid-" + id)) << QString("id") << QJsonValue(id);
        for (int length : {0, 31, 33, 200})
            QTest::newRow(qPrintable(QString("nonce-%1").arg(length)))
                << QString("challenge")
                << QJsonValue(QString::fromLatin1(CloudCrypto::base64Url(QByteArray(length, 'k'))));
        QTest::newRow("padding") << QString("challenge")
                                 << QJsonValue(challenge().value("challenge").toString() + '=');
        auto noncanonical = challenge().value("challenge").toString();
        noncanonical.back() = QChar('t'); // same decoded final byte, nonzero unused bits
        QTest::newRow("noncanonical-bits") << QString("challenge") << QJsonValue(noncanonical);
    }

    void rejectsMalformedOrUnboundResponse() {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        auto reply = challenge();
        reply.insert(field, value);
        QVERIFY(machine_challenge::signingBytes(reply, "machine-synthetic", 1).isEmpty());

        // Exercise the real HTTP credential path too: invalid input never produces a
        // credential request or a legacy retry, not merely a helper validation error.
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite"));
        CodexAdapter provider("/nonexistent/fake-provider");
        SessionHost host(&journal, &provider);
        r03::Transport transport;
        CloudControlClient client(&journal, &host);
        Access::configure(client, transport);
        transport.status = 201;
        transport.response = QJsonDocument(reply).toJson(QJsonDocument::Compact);
        std::optional<bool> ready;
        Access::credential(client, [&](bool ok, const QString&) { ready = ok; });
        QTRY_VERIFY(ready.has_value());
        QVERIFY(!*ready);
        QCOMPARE(transport.requests.size(), 1);
        QCOMPARE(transport.requests.first().url().path(), QString("/v2/daemon/challenges"));
    }

    void validatesLocalIdentityAndAcceptsMaximumBoundaries() {
        auto reply = challenge();
        QVERIFY(machine_challenge::signingBytes(reply, "machine-synthetic", 0).isEmpty());
        QVERIFY(machine_challenge::signingBytes(reply, "machine-synthetic", -1).isEmpty());
        reply.insert("id", QString(128, 'A'));
        reply.insert("machineId", QString(128, '_'));
        reply.insert("keyVersion", std::numeric_limits<int>::max());
        QVERIFY(!machine_challenge::signingBytes(reply, QString(128, '_'),
                                                 std::numeric_limits<int>::max())
                     .isEmpty());
        reply.insert("machineId", "invalid/machine");
        QVERIFY(machine_challenge::signingBytes(reply, "invalid/machine",
                                                std::numeric_limits<int>::max())
                    .isEmpty());
    }

    void sendsOnlyV2AndSignsTheBoundDomain() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite"));
        CodexAdapter provider("/nonexistent/fake-provider");
        SessionHost host(&journal, &provider);
        r03::Transport transport;
        CloudControlClient client(&journal, &host);
        Access::configure(client, transport);
        transport.status = 201;
        transport.responseForPath.insert("/v2/daemon/challenges",
                                         QJsonDocument(challenge()).toJson(QJsonDocument::Compact));
        transport.responseForPath.insert(
            "/v2/daemon/credentials",
            QJsonDocument(
                QJsonObject{{"accessToken", "synthetic-v2-token"},
                            {"expiresAt",
                             QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODate)}})
                .toJson(QJsonDocument::Compact));
        std::optional<bool> ready;
        Access::credential(client, [&](bool ok, const QString&) { ready = ok; });
        QTRY_VERIFY(ready.has_value());
        QVERIFY(*ready);
        QCOMPARE(transport.requests.size(), 2);
        for (const auto& encoded : transport.bodies) {
            const auto body = QJsonDocument::fromJson(encoded).object();
            QCOMPARE(body.value("signatureVersion"), QJsonValue(2));
            QCOMPARE(body.value("signatureAlgorithm"), QJsonValue("Ed25519"));
        }
        QCOMPARE(QJsonDocument::fromJson(transport.bodies.first()).object().value("machineId"),
                 QJsonValue("machine-synthetic"));
        QCOMPARE(QJsonDocument::fromJson(transport.bodies.last()).object().value("challengeId"),
                 QJsonValue("challenge_1"));
        const auto signature = CloudCrypto::fromBase64Url(QString::fromLatin1(
            transport.requests.last().rawHeader("X-KetPlus-Machine-Signature")));
        const auto key = CloudCrypto::fromBase64Url(
            Access::crypto(client).publicKeys().value("signingPublicKey").toString());
        // Literal bytes independently of the serializer under test.
        QByteArray bytes(
            "[\"ketplus\",2,\"daemon-challenge\",\"challenge_1\",\"machine-synthetic\",1]");
        bytes.append('\0');
        bytes.append(QByteArray(32, 'k'));
        QVERIFY(verify(key, bytes, signature));
        QVERIFY(!verify(key, QByteArray(32, 'k'), signature));
    }
};

QTEST_GUILESS_MAIN(MachineChallengeTest)
#include "MachineChallengeTest.moc"
