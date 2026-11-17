// CloudCrypto's existing static API against ketplus_control_crypto_primitives on the same input,
// and CloudCrypto's stored-key signing against the passed-key primitive.
#include "cloud-privacy/MemoryCredentials.h"
#include "cloud/CloudCrypto.h"
#include "cloud/CryptoPrimitives.h"

#include <QtTest>

#include <limits>

using ketplus::daemon::CloudCrypto;
using ketplus::daemon::CredentialStore;
namespace primitives = ketplus::control_crypto;

class ControlCryptoParityTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { r03::resetCredentials(); }

    void canonicalMatches_data() {
        QTest::addColumn<QJsonArray>("values");
        QTest::newRow("empty") << QJsonArray();
        QTest::newRow("signature tuple")
            << QJsonArray{"ketplus", 2, "webrtc-answer", "pc_1", "workspace",
                          "machine", 1, "device",        "abc",  "2026-11-17T00:00:00.000Z",
                          "def"};
        QTest::newRow("unicode") << QJsonArray{
            QStringLiteral("été"),        QStringLiteral("☃"),
            QStringLiteral("\U0001F600"), QStringLiteral("quote\" back\\ \u0001\n"),
            QStringLiteral("中文"),       QString()};
        QTest::newRow("numbers") << QJsonArray{0,
                                               -0.0,
                                               1.5,
                                               -3,
                                               1e300,
                                               4.9e-324,
                                               9007199254740993.0,
                                               qint64(9007199254740991),
                                               std::numeric_limits<qint64>::max(),
                                               std::numeric_limits<qint64>::min(),
                                               0.1};
        QTest::newRow("null and bool") << QJsonArray{QJsonValue(), true, false, QJsonValue()};
        QTest::newRow("nested") << QJsonArray{
            QJsonArray{QJsonArray{QJsonArray{"deep", QJsonValue()}}, QJsonArray()},
            QJsonObject{{"b", 1}, {"a", QJsonArray{2, "x"}}}, "tail"};
    }
    void canonicalMatches() {
        QFETCH(QJsonArray, values);
        QCOMPARE(primitives::canonical(values), CloudCrypto::canonical(values));
    }

    void base64UrlMatches() {
        QList<QByteArray> inputs{QByteArray(),
                                 QByteArray("f"),
                                 QByteArray("fo"),
                                 QByteArray("foo"),
                                 QByteArray::fromHex("fbfffe00ff"),
                                 QStringLiteral("é\U0001F600").toUtf8()};
        QByteArray all;
        for (int i = 0; i < 256; ++i)
            all.append(char(i));
        inputs.append(all);
        for (const auto& bytes : inputs) {
            QCOMPARE(primitives::base64Url(bytes), CloudCrypto::base64Url(bytes));
            const auto text = QString::fromLatin1(CloudCrypto::base64Url(bytes));
            QCOMPARE(primitives::fromBase64Url(text), CloudCrypto::fromBase64Url(text));
        }
        // Lenient decoding of padding, the standard alphabet and junk stays as it was.
        for (const auto& text : {QStringLiteral("Zg=="), QStringLiteral("+/+/"),
                                 QStringLiteral("a b!"), QStringLiteral("éé"), QStringLiteral("Z")})
            QCOMPARE(primitives::fromBase64Url(text), CloudCrypto::fromBase64Url(text));
    }

    void keyFingerprintMatches() {
        CredentialStore store("parity");
        CloudCrypto crypto(&store);
        const auto keys = crypto.publicKeys();
        QVERIFY(!keys.isEmpty());
        QList<QJsonObject> sets{keys, QJsonObject{}};
        auto shortKey = keys;
        shortKey["signingPublicKey"] =
            QString::fromLatin1(primitives::base64Url(QByteArray(31, 'a')));
        sets.append(shortKey);
        auto unicode = keys;
        unicode["agreementPublicKey"] = QStringLiteral("é☃");
        sets.append(unicode);
        auto number = keys;
        number["signingPublicKey"] = 42;
        sets.append(number);
        for (const auto& set : sets)
            QCOMPARE(primitives::keyFingerprint(set), CloudCrypto::keyFingerprint(set));
        QVERIFY(!CloudCrypto::keyFingerprint(keys).isEmpty());
    }

    void storedKeySigningMatchesThePassedKeyPrimitive() {
        // RFC 8032 TEST 2, stored as CloudCrypto's signing identity.
        const auto seed =
            QByteArray::fromHex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
        r03::credentials.insert("parity/signing-private-key", seed);
        CredentialStore store("parity");
        CloudCrypto crypto(&store);
        const auto message = QByteArray::fromHex("72");
        const auto signature = crypto.sign(message);
        QCOMPARE(signature, primitives::signEd25519(seed, message));
        QCOMPARE(signature.toHex(),
                 QByteArray("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1"
                            "e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"));
        const auto signing =
            CloudCrypto::fromBase64Url(crypto.publicKeys().value("signingPublicKey").toString());
        QCOMPARE(signing.toHex(),
                 QByteArray("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"));
        QVERIFY(primitives::verifyEd25519(signing, message, signature));
    }

    void generatedIdentityIsStoredOnceAndUsable() {
        CredentialStore store("parity");
        CloudCrypto crypto(&store);
        const auto keys = crypto.publicKeys();
        QCOMPARE(r03::credentials.value("parity/signing-private-key").size(), 32);
        QCOMPARE(r03::credentials.value("parity/agreement-private-key").size(), 32);
        QCOMPARE(crypto.publicKeys(), keys);
        const auto signature = crypto.sign("bytes");
        QVERIFY(primitives::verifyEd25519(
            CloudCrypto::fromBase64Url(keys.value("signingPublicKey").toString()), "bytes",
            signature));
    }
};

QTEST_GUILESS_MAIN(ControlCryptoParityTest)
#include "ControlCryptoParityTest.moc"
