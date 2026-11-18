// CloudCrypto's existing static API against ketplus_control_crypto_primitives on the same input,
// and CloudCrypto's stored-key signing against the passed-key primitive.
#include "cloud-privacy/MemoryCredentials.h"
#include "cloud/CloudCrypto.h"
#include "cloud/CryptoPrimitives.h"

#include <QtTest>

#include <limits>
#include <utility>

using ketplus::daemon::CloudCrypto;
using ketplus::daemon::CredentialStore;
namespace primitives = ketplus::control_crypto;

class ControlCryptoParityTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { r03::resetCredentials(); }

    // Expected bytes were produced by the CloudCrypto implementation before this extraction
    // (origin/integration e52b057f), on exactly these inputs; both APIs must still produce them.
    void canonicalMatchesThePreviousImplementation_data() {
        QTest::addColumn<QJsonArray>("values");
        QTest::addColumn<QByteArray>("expected");
        QTest::newRow("empty") << QJsonArray() << QByteArray::fromHex("5b5d");
        QTest::newRow("signature tuple")
            << QJsonArray{"ketplus", 2, "webrtc-answer", "pc_1", "workspace",
                          "machine", 1, "device",        "abc",  "2026-11-17T00:00:00.000Z",
                          "def"}
            << QByteArray::fromHex("5b226b6574706c7573222c322c227765627274632d616e73776572222c22706"
                                   "35f31222c22776f72"
                                   "6b7370616365222c226d616368696e65222c312c22646576696365222c22616"
                                   "263222c2232303236"
                                   "2d31312d31375430303a30303a30302e3030305a222c22646566225d");
        QTest::newRow("unicode") << QJsonArray{QStringLiteral("\u00E9t\u00E9"),
                                               QStringLiteral("\u2603"),
                                               QStringLiteral("\U0001F600"),
                                               QStringLiteral("quote\" back\\ \u0001\n"),
                                               QStringLiteral("\u4E2D\u6587"),
                                               QString()}
                                 << QByteArray::fromHex("5b22c3a974c3a9222c22e29883222c22f09f988022"
                                                        "2c2271756f74655c22206261636b5c5c205c75"
                                                        "303030315c6e222c22e4b8ade69687222c22225d");
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
                                               0.1}
                                 << QByteArray::fromHex(
                                        "5b302c302c312e352c2d332c31652b3330302c35652d3332342c393030"
                                        "3731393932353437343039"
                                        "39322c393030373139393235343734303939312c393232333337323033"
                                        "363835343737353830372c"
                                        "2d393232333337323033363835343737353830382c302e315d");
        QTest::newRow("null and bool")
            << QJsonArray{QJsonValue(), true, false, QJsonValue()}
            << QByteArray::fromHex("5b6e756c6c2c747275652c66616c73652c6e756c6c5d");
        // A braced list holding one array of the same type may copy it instead of nesting it
        // (MSVC does), so the nesting is built with explicit appends.
        QJsonArray leaf{"deep", QJsonValue()};
        QJsonArray wrapped;
        wrapped.append(leaf);
        QJsonArray first;
        first.append(wrapped);
        first.append(QJsonArray());
        QJsonArray nested;
        nested.append(first);
        nested.append(QJsonObject{{"b", 1}, {"a", QJsonArray{2, "x"}}});
        nested.append("tail");
        QTest::newRow("nested")
            << nested
            << QByteArray::fromHex("5b5b5b5b2264656570222c6e756c6c5d5d2c5b5d5d2c7b2261223a5b322c227"
                                   "8225d2c2262223a31"
                                   "7d2c227461696c225d");
    }
    void canonicalMatchesThePreviousImplementation() {
        QFETCH(QJsonArray, values);
        QFETCH(QByteArray, expected);
        QCOMPARE(CloudCrypto::canonical(values), expected);
        QCOMPARE(primitives::canonical(values), expected);
    }

    void base64UrlMatchesThePreviousImplementation() {
        QByteArray all;
        for (int i = 0; i < 256; ++i)
            all.append(char(i));
        const QList<std::pair<QByteArray, QByteArray>> encoded{
            {all,
             QByteArray::fromHex(
                 "41414543417751464267634943516f4c4441304f4478415245684d554652595847426b6147787764"
                 "486838674953496a4a43556d4a7967704b6973734c5334764d4445794d7a51314e6a63344f546f37"
                 "5044302d50304242516b4e4552555a4853456c4b5330784e546b395155564a54564656575631685a"
                 "576c746358563566594746695932526c5a6d646f615770726247317562334278636e4e3064585a33"
                 "65486c3665337839666e2d4167594b44684957476834694a696f754d6a5936506b4a47536b355356"
                 "6c7065596d5a71626e4a32656e3643686f714f6b7061616e714b6d717136797472712d7773624b7a"
                 "744c573274376935757275387662365f774d484377385446787366497963724c7a4d334f7a394452"
                 "3074505531646258324e6e6132397a6433745f6734654c6a354f586d352d6a703675767337653776"
                 "38504879385f5431397666342d6672375f50332d5f77")},
            {QStringLiteral("\u00E9\U0001F600").toUtf8(), QByteArray::fromHex("77366e776e356941")},
            {QByteArray::fromHex("fbfffe00ff"), QByteArray::fromHex("2d5f5f2d415038")}};
        for (const auto& [bytes, expected] : encoded) {
            QCOMPARE(CloudCrypto::base64Url(bytes), expected);
            QCOMPARE(primitives::base64Url(bytes), expected);
            QCOMPARE(CloudCrypto::fromBase64Url(QString::fromLatin1(expected)), bytes);
            QCOMPARE(primitives::fromBase64Url(QString::fromLatin1(expected)), bytes);
        }
        // Lenient decoding of padding, the standard alphabet and junk stays as it was.
        const QList<std::pair<QString, QByteArray>> decoded{
            {QStringLiteral("Zg=="), QByteArray::fromHex("66")},
            {QStringLiteral("+/+/"), QByteArray::fromHex("")},
            {QStringLiteral("a b!"), QByteArray::fromHex("69")},
            {QStringLiteral("\u00E9\u00E9"), QByteArray::fromHex("")},
            {QStringLiteral("Z"), QByteArray::fromHex("")}};
        for (const auto& [text, expected] : decoded) {
            QCOMPARE(CloudCrypto::fromBase64Url(text), expected);
            QCOMPARE(primitives::fromBase64Url(text), expected);
        }
    }

    void keyFingerprintMatchesThePreviousImplementation() {
        const QJsonObject keys{
            {"signingPublicKey", QStringLiteral("11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo")},
            {"agreementPublicKey", QStringLiteral("AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA")}};
        const auto expected = QString::fromLatin1(QByteArray::fromHex(
            "55674353524f735f57564463514856424858494569467975665375735379304937506d6159776f36"
            "6d6b77"));
        auto padded = keys;
        padded["signingPublicKey"] = QStringLiteral("11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo=");
        auto shortKey = keys;
        shortKey["signingPublicKey"] =
            QString::fromLatin1(primitives::base64Url(QByteArray(31, 'a')));
        auto unicode = keys;
        unicode["agreementPublicKey"] = QStringLiteral("\u00E9\u2603");
        auto number = keys;
        number["signingPublicKey"] = 42;
        const QList<std::pair<QJsonObject, QString>> cases{
            {keys, expected},     {padded, expected},  {shortKey, QString()},
            {unicode, QString()}, {number, QString()}, {QJsonObject{}, QString()}};
        for (const auto& [set, want] : cases) {
            QCOMPARE(CloudCrypto::keyFingerprint(set), want);
            QCOMPARE(primitives::keyFingerprint(set), want);
        }
        QCOMPARE(primitives::keyFingerprint(
                     primitives::fromBase64Url(keys["signingPublicKey"].toString()),
                     primitives::fromBase64Url(keys["agreementPublicKey"].toString())),
                 expected);
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
