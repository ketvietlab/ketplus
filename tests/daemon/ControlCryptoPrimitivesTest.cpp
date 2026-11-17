// ketplus_control_crypto_primitives alone: this executable links only that target and Qt Test.
#include "cloud/CryptoPrimitives.h"

#include <QtTest>

#include <openssl/evp.h>

using namespace ketplus::control_crypto;

namespace {
QByteArray hex(const char* text) { return QByteArray::fromHex(text); }
} // namespace

class ControlCryptoPrimitivesTest final : public QObject {
    Q_OBJECT
  private slots:
    void ed25519MatchesRfc8032_data() {
        QTest::addColumn<QByteArray>("privateKey");
        QTest::addColumn<QByteArray>("publicKey");
        QTest::addColumn<QByteArray>("message");
        QTest::addColumn<QByteArray>("signature");
        // RFC 8032 section 7.1.
        QTest::newRow("TEST 1")
            << hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
            << hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
            << QByteArray()
            << hex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590"
                   "a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
        QTest::newRow("TEST 2")
            << hex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb")
            << hex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c") << hex("72")
            << hex("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e"
                   "15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
        QTest::newRow("TEST 3")
            << hex("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7")
            << hex("fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025")
            << hex("af82")
            << hex("6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d"
                   "16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a");
        QTest::newRow("TEST SHA(abc)")
            << hex("833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42")
            << hex("ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf")
            << hex("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274f"
                   "c1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f")
            << hex("dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac"
                   "90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704");
    }
    void ed25519MatchesRfc8032() {
        QFETCH(QByteArray, privateKey);
        QFETCH(QByteArray, publicKey);
        QFETCH(QByteArray, message);
        QFETCH(QByteArray, signature);
        QString error;
        QCOMPARE(ketplus::control_crypto::publicKey(privateKey, EVP_PKEY_ED25519, &error),
                 publicKey);
        QCOMPARE(signEd25519(privateKey, message, &error), signature);
        QVERIFY(error.isEmpty());
        QVERIFY(verifyEd25519(publicKey, message, signature));

        // Any change to the message, the signature or the key fails, without a crash.
        QVERIFY(!verifyEd25519(publicKey, message + "x", signature));
        if (!message.isEmpty()) {
            auto changed = message;
            changed[0] = char(changed.at(0) ^ 0x01);
            QVERIFY(!verifyEd25519(publicKey, changed, signature));
        }
        for (const int at : {0, 31, 32, 63}) {
            auto changed = signature;
            changed[at] = char(changed.at(at) ^ 0x80);
            QVERIFY(!verifyEd25519(publicKey, message, changed));
        }
        auto otherKey = publicKey;
        otherKey[5] = char(otherKey.at(5) ^ 0x01);
        QVERIFY(!verifyEd25519(otherKey, message, signature));
        QVERIFY(!verifyEd25519(generateEd25519().publicKey, message, signature));
    }

    void malformedKeysAndSignaturesAreRefused() {
        const auto pair = generateEd25519();
        const QByteArray message("message");
        const auto signature = signEd25519(pair.privateKey, message);
        QCOMPARE(signature.size(), 64);
        QVERIFY(verifyEd25519(pair.publicKey, message, signature));
        for (const auto& key : {QByteArray(), pair.publicKey.left(31), pair.publicKey + 'x',
                                QByteArray(32, '\0'), QByteArray(64, 'k')})
            QVERIFY(!verifyEd25519(key, message, signature));
        for (const auto& bad : {QByteArray(), signature.left(63), signature + 'x',
                                QByteArray(64, '\0'), QByteArray(128, 's')})
            QVERIFY(!verifyEd25519(pair.publicKey, message, bad));
        for (const auto& key : {QByteArray(), pair.privateKey.left(31), pair.privateKey + 'x'}) {
            QString error;
            QVERIFY(signEd25519(key, message, &error).isEmpty());
            QVERIFY(error.startsWith(QStringLiteral("Unable to sign content.")));
            QVERIFY(!error.contains(QString::fromLatin1(pair.privateKey.toHex())));
            QVERIFY(ketplus::control_crypto::publicKey(key, EVP_PKEY_ED25519).isEmpty());
        }
    }

    void generatedKeyPairsSignAndVerify() {
        QString error;
        const auto first = generateEd25519(&error);
        const auto second = generateEd25519(&error);
        QVERIFY(error.isEmpty());
        QCOMPARE(first.privateKey.size(), 32);
        QCOMPARE(first.publicKey.size(), 32);
        QVERIFY(first.privateKey != second.privateKey);
        QCOMPARE(ketplus::control_crypto::publicKey(first.privateKey, EVP_PKEY_ED25519),
                 first.publicKey);
        const QByteArray message = canonical({"ketplus", 2, "pairing"});
        const auto signature = signEd25519(first.privateKey, message);
        QVERIFY(verifyEd25519(first.publicKey, message, signature));
        QVERIFY(!verifyEd25519(second.publicKey, message, signature));
        // Ed25519 is deterministic.
        QCOMPARE(signEd25519(first.privateKey, message), signature);

        const auto agreement = generatePrivateKey(EVP_PKEY_X25519, &error);
        QCOMPARE(agreement.size(), 32);
        QCOMPARE(ketplus::control_crypto::publicKey(agreement, EVP_PKEY_X25519).size(), 32);
    }

    void base64UrlRoundTripsWithoutPadding_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<QByteArray>("encoded");
        QTest::newRow("empty") << QByteArray() << QByteArray();
        QTest::newRow("one") << QByteArray("f") << QByteArray("Zg");
        QTest::newRow("two") << QByteArray("fo") << QByteArray("Zm8");
        QTest::newRow("three") << QByteArray("foo") << QByteArray("Zm9v");
        QTest::newRow("url alphabet") << hex("fbfffe") << QByteArray("-__-");
    }
    void base64UrlRoundTripsWithoutPadding() {
        QFETCH(QByteArray, bytes);
        QFETCH(QByteArray, encoded);
        QCOMPARE(base64Url(bytes), encoded);
        QCOMPARE(fromBase64Url(QString::fromLatin1(encoded)), bytes);
    }

    void canonicalIsCompactJson() {
        QCOMPARE(canonical({"ketplus", 2, "webrtc-offer", QJsonValue(), true, -3, 1.5}),
                 QByteArray(R"(["ketplus",2,"webrtc-offer",null,true,-3,1.5])"));
        QCOMPARE(canonical({QJsonArray{"a", QJsonArray{1, QJsonValue()}}, QJsonArray()}),
                 QByteArray(R"([["a",[1,null]],[]])"));
        QCOMPARE(canonical({QStringLiteral("é☃")}), QByteArray("[\"\xc3\xa9\xe2\x98\x83\"]"));
        QCOMPARE(canonical({}), QByteArray("[]"));
    }

    void keyFingerprintHashesBothPublicKeys() {
        const auto signing =
            hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
        QByteArray agreement;
        for (char i = 1; i <= 32; ++i)
            agreement.append(i);
        const QString expected = QStringLiteral("UgCSROs_WVDcQHVBHXIEiFyufSusSy0I7PmaYwo6mkw");
        QCOMPARE(keyFingerprint(signing, agreement), expected);
        QCOMPARE(
            keyFingerprint(QJsonObject{
                {"signingPublicKey", QStringLiteral("11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo")},
                {"agreementPublicKey",
                 QStringLiteral("AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA")}}),
            expected);
        QVERIFY(keyFingerprint(signing.left(31), agreement).isEmpty());
        QVERIFY(keyFingerprint(signing, agreement + 'x').isEmpty());
        QVERIFY(keyFingerprint(QJsonObject{}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(ControlCryptoPrimitivesTest)
#include "ControlCryptoPrimitivesTest.moc"
