#include "pairing/PairingIdentity.h"
#include "pairing/PairingPins.h"
#include "pairing/PairingSignatures.h"
#include "pairing/PairingToken.h"

#include <QDebug>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTest>

#include <functional>

using namespace ketplus::runner::pairing;

// Expected values come from Python's standard library (hashlib, base64, json) and the
// OpenSSL 3 command line (`pkey -pubout` for public keys, `pkeyutl -sign -rawin` for Ed25519
// signatures), never from this library.
namespace {

struct Vector {
    const char* runnerSeed;
    const char* runnerPublicKey;
    const char* desktopSeed;
    const char* desktopSigningPublicKey;
    const char* desktopAgreementPublicKey;
    const char* runnerFingerprint;
    const char* desktopFingerprint;
    const char* ticketId;
    const char* workspaceId;
    const char* machineId;
    int machineKeyVersion;
    const char* runnerId;
    int runnerKeyVersion;
    const char* digestInput;
    const char* digest;
    const char* code;
};

const Vector kVectors[] = {
    {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
     "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8",
     "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
     "29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd7",
     "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f",
     "Vkdap1RjR0wChd9dvyvKtz2mUTWIOem3dIGy6rEHcIw", "mfkwAn4_bsM2Q_FIUUrRP7n3NMXuJlGeyWfkL4D51CI",
     "rpt_0123456789abcdef", "ws_alpha", "m_desktop01", 1, "run_aaaaaaaaaaaa", 1,
     R"(["ketplus",2,"runner-pair","rpt_0123456789abcdef","ws_alpha","m_desktop01",1,)"
     R"("mfkwAn4_bsM2Q_FIUUrRP7n3NMXuJlGeyWfkL4D51CI","run_aaaaaaaaaaaa",1,)"
     R"("Vkdap1RjR0wChd9dvyvKtz2mUTWIOem3dIGy6rEHcIw"])",
     "MBUNF1XvxaTzhMIN0WVNaNV3Si0hU8qpsGnsAmkL0kg", "GAKQ2-F2V57"},
    {"1111111111111111111111111111111111111111111111111111111111111111",
     "d04ab232742bb4ab3a1368bd4615e4e6d0224ab71a016baf8520a332c9778737",
     "2222222222222222222222222222222222222222222222222222222222222222",
     "a09aa5f47a6759802ff955f8dc2d2a14a5c99d23be97f864127ff9383455a4f0",
     "3333333333333333333333333333333333333333333333333333333333333333",
     "ELpoLIrRNROXHotWiBqri9cCu4B3luyoGTLHNalNbm0", "lvKCvSMj8OE5JWrYUhr5unCPg68Zb9pyRSQup2gg5u0",
     "rpt_ffffffffffffffff", "ws_beta", "m_desktop02", 3, "run_bbbbbbbbbbbb", 2,
     R"(["ketplus",2,"runner-pair","rpt_ffffffffffffffff","ws_beta","m_desktop02",3,)"
     R"("lvKCvSMj8OE5JWrYUhr5unCPg68Zb9pyRSQup2gg5u0","run_bbbbbbbbbbbb",2,)"
     R"("ELpoLIrRNROXHotWiBqri9cCu4B3luyoGTLHNalNbm0"])",
     "MnPB2QFoSumxuNCM9edPBJnPRxePPGGXkNePMjo-JtM", "GJZ4D-WIBNB"},
    {"fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0efeeedecebeae9e8e7e6e5e4e3e2e1e0",
     "bafc71bead3ac5e4b63e9c8216ee71a34aaec65722eedbca728b4e9b3ccce396",
     "c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7",
     "32b53e882e3daac180d7a5f6224d61b6401b43f901db09f0e4d84ce4e4a2718b",
     "6465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f80818283",
     "JkIXf4BNGrbH_T12eDmagcgC2AGEGetYUcJsOqMM3kg", "RIB4KbqtZs82QnDpGW9T0syYvjxNDh7eNHtj5jO9sdI",
     "rpt_x", "ws_\xc3\xa9t\xc3\xa9", "m_3", 42, "run_c", 7,
     "[\"ketplus\",2,\"runner-pair\",\"rpt_x\",\"ws_\xc3\xa9t\xc3\xa9\",\"m_3\",42,"
     "\"RIB4KbqtZs82QnDpGW9T0syYvjxNDh7eNHtj5jO9sdI\",\"run_c\",7,"
     "\"JkIXf4BNGrbH_T12eDmagcgC2AGEGetYUcJsOqMM3kg\"]",
     "OR-HcKrXy6Fn4Ax0tMwloC9IjCcJur87Kaf-yQAX6rc", "HEPYO-4FK27"},
};

// Signed payloads of vector 0, signed with its runner or desktop seed.
const char* const issuedAt = "2026-11-18T03:00:00.000Z";
const char* const offerSdp = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\n";
const char* const answerSdp = "v=0\r\no=- 3 4 IN IP4 127.0.0.1\r\n";
const char* const connectionId = "pc_abcdefghijklmnopqrstuvwx";
const char* const nonce = "nonce_0123456789";
const char* const jobId = "job_42";

const char* const registerBytes =
    R"(["ketplus",2,"runner-register","rpt_0123456789abcdef","ws_alpha","m_desktop01",1,)"
    R"("Vkdap1RjR0wChd9dvyvKtz2mUTWIOem3dIGy6rEHcIw","2026-11-18T03:00:00.000Z"])";
const char* const registerSignature =
    "-xCdcn-5FJuZ4AQxchXCbjnGD-CocJlesP9m0hLtGYGzJMQO3ufAA5q9sMHErs6eQZxieFXC6MfkACRUIxduCw";
const char* const approvalBytes = R"(["ketplus",2,"runner-pair-approve","rpt_0123456789abcdef",)"
                                  R"("MBUNF1XvxaTzhMIN0WVNaNV3Si0hU8qpsGnsAmkL0kg"])";
const char* const approvalSignature =
    "xIZ3gBd5fOajEm_hbRoBgS954VKwPHzuM5YgCSND95X-Y0hdy5fWwJwrK3mvxp5dtnAifXiuiUZCGdLdNBtLDA";
const char* const challengeBytes =
    R"(["ketplus",2,"runner-challenge","run_aaaaaaaaaaaa",1,"nonce_0123456789",)"
    R"("2026-11-18T03:00:00.000Z"])";
const char* const challengeSignature =
    "-tWMq7lJlY8sPMItacf3boJfN2j1GMfNLSpIM_p8wMqbqnOBq50Z8Y1F-lxOqbeQVKxUkakKHVzyqKR1yzbIBg";
const char* const offerBytes =
    R"(["ketplus",2,"runner-offer","pc_abcdefghijklmnopqrstuvwx","ws_alpha","m_desktop01",)"
    R"("run_aaaaaaaaaaaa",1,"job_42",1,"2026-11-18T03:00:00.000Z",)"
    R"("4c1b50f0e8a0a87e3f8b65de77023b97e3d974085dfb690332f1f839b34b0fbc"])";
const char* const offerSignature =
    "4XAl_7mz9GmAsHmWHMsFJ8ZGI8DHAUIPWfn5Rk_3_utVEsqJM5R9G9UX0xOA6IWzGj3jrWi-MIFCzklp5cG3CA";
const char* const answerBytes =
    R"(["ketplus",2,"runner-answer","pc_abcdefghijklmnopqrstuvwx","ws_alpha","m_desktop01",1,)"
    R"("run_aaaaaaaaaaaa","job_42",)"
    R"("4c1b50f0e8a0a87e3f8b65de77023b97e3d974085dfb690332f1f839b34b0fbc",)"
    R"("2026-11-18T03:00:00.000Z",)"
    R"("ff0f5415b17f667014df0df3b418bacf239d2ae04c68696fea7b5cd651a5d696"])";
const char* const answerSignature =
    "sLgP35gx9obUbcN0GR_JZzs-o0d7-qw80LH9mKs-SRR65A3S_x-ucQjWGKtZG4nDZ051KinWiGG8W7n8ahZdAA";

const char* const secret = "SECRET-ticket-secret-value";

QByteArray hex(const char* text) { return QByteArray::fromHex(text); }
QString str(const char* text) { return QString::fromUtf8(text); }
QByteArray sig(const char* text) {
    return QByteArray::fromBase64(text, QByteArray::Base64UrlEncoding);
}

QDateTime at(const char* text) { return QDateTime::fromString(str(text), Qt::ISODateWithMs); }

PairingDigestInput digestInput(const Vector& v) {
    return {str(v.ticketId),     str(v.workspaceId),        str(v.machineId),
            v.machineKeyVersion, str(v.desktopFingerprint), str(v.runnerId),
            v.runnerKeyVersion,  str(v.runnerFingerprint)};
}

PairingTokenFields tokenFields(const Vector& v, bool addDesktop) {
    PairingTokenFields fields;
    fields.ticketId = str(v.ticketId);
    fields.ticketSecret = str(secret);
    fields.workspaceId = str(v.workspaceId);
    fields.machineId = str(v.machineId);
    fields.machineKeyVersion = v.machineKeyVersion;
    fields.desktopFingerprint = str(v.desktopFingerprint);
    if (addDesktop)
        fields.runnerId = str(v.runnerId);
    return fields;
}

QByteArray tokenOf(const QByteArray& json) {
    return "kpr1." + json.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QString debugText(const PairingToken& token) {
    QString text;
    QDebug(&text) << token;
    return text;
}

const Vector& v0 = kVectors[0];

RegisterPayload registerPayload() {
    return {str(v0.ticketId),    str(v0.workspaceId),       str(v0.machineId),
            v0.runnerKeyVersion, str(v0.runnerFingerprint), str(issuedAt)};
}
ApprovalPayload approvalPayload() { return {str(v0.ticketId), str(v0.digest)}; }
ChallengePayload challengePayload() {
    return {str(v0.runnerId), v0.runnerKeyVersion, str(nonce), str(issuedAt)};
}
OfferPayload offerPayload() {
    return {str(connectionId),
            str(v0.workspaceId),
            str(v0.machineId),
            str(v0.runnerId),
            v0.runnerKeyVersion,
            str(jobId),
            1,
            str(issuedAt),
            str(offerSdp)};
}
AnswerPayload answerPayload() {
    return {str(connectionId),    str(v0.workspaceId), str(v0.machineId),
            v0.machineKeyVersion, str(v0.runnerId),    str(jobId),
            str(offerSdp),        str(issuedAt),       str(answerSdp)};
}

VerifyingKey runnerKey() { return {hex(v0.runnerPublicKey), v0.runnerKeyVersion}; }
VerifyingKey desktopKey() { return {hex(v0.desktopSigningPublicKey), v0.machineKeyVersion}; }

ReportedDesktop reportedDesktop() {
    return {str(v0.machineId), v0.machineKeyVersion, hex(v0.desktopSigningPublicKey),
            hex(v0.desktopAgreementPublicKey)};
}

// The runner's pin after claiming vector 0's token, with the digest it computed itself.
RunnerPin pendingPin() {
    const auto token = PairingToken::create(tokenFields(v0, false));
    const auto pin =
        RunnerPin::pending(token.value(), reportedDesktop(), pairingCode(digestInput(v0)).digest);
    return pin.value();
}

using DigestChange = std::function<void(PairingDigestInput&)>;
const std::pair<const char*, DigestChange> digestChanges[] = {
    {"ticketId", [](PairingDigestInput& i) { i.ticketId += QLatin1Char('x'); }},
    {"workspaceId", [](PairingDigestInput& i) { i.workspaceId += QLatin1Char('x'); }},
    {"machineId", [](PairingDigestInput& i) { i.machineId += QLatin1Char('x'); }},
    {"machineKeyVersion", [](PairingDigestInput& i) { ++i.machineKeyVersion; }},
    {"desktopFingerprint",
     [](PairingDigestInput& i) { i.desktopFingerprint = str(kVectors[1].desktopFingerprint); }},
    {"runnerId", [](PairingDigestInput& i) { i.runnerId += QLatin1Char('x'); }},
    {"runnerKeyVersion", [](PairingDigestInput& i) { ++i.runnerKeyVersion; }},
    {"runnerFingerprint",
     [](PairingDigestInput& i) { i.runnerFingerprint = str(kVectors[1].runnerFingerprint); }},
};

} // namespace

class RunnerPairingTest : public QObject {
    Q_OBJECT

  private slots:
    void tokenRoundTrip();
    void tokenRejects_data();
    void tokenRejects();
    void tokenNeverPrinted();
    void vectors_data();
    void vectors();
    void digestChangesWithEachField_data();
    void digestChangesWithEachField();
    void desktopKeyCheck_data();
    void desktopKeyCheck();
    void signaturesMatchVectors();
    void signaturesVerify();
    void signatureFieldChangesFail();
    void signatureWrongKeyOrVersionFails();
    void approvalChecksAgainstPendingPin();
    void issuedAtWindow_data();
    void issuedAtWindow();
    void constantTimeEqualsAndBase64Url();
    void pinsRoundTrip();
    void corruptPinsFail_data();
    void corruptPinsFail();
};

void RunnerPairingTest::tokenRoundTrip() {
    for (const bool addDesktop : {false, true}) {
        const auto created = PairingToken::create(tokenFields(v0, addDesktop));
        QVERIFY(created.ok());
        const QByteArray text = created.value().encode();
        QVERIFY(text.startsWith("kpr1."));
        QVERIFY(text.size() <= maxPairingTokenBytes);
        // A trailing newline from a file or stdin is ignored.
        const auto parsed = PairingToken::parse(text + "\n");
        QVERIFY(parsed.ok());
        const PairingTokenFields& fields = parsed.value().fields();
        QCOMPARE(fields.ticketId, str(v0.ticketId));
        QCOMPARE(fields.ticketSecret, str(secret));
        QCOMPARE(fields.workspaceId, str(v0.workspaceId));
        QCOMPARE(fields.machineId, str(v0.machineId));
        QCOMPARE(fields.machineKeyVersion, v0.machineKeyVersion);
        QCOMPARE(fields.desktopFingerprint, str(v0.desktopFingerprint));
        QCOMPARE(fields.runnerId.has_value(), addDesktop);
        if (addDesktop)
            QCOMPARE(*fields.runnerId, str(v0.runnerId));
        QCOMPARE(parsed.value().kind(),
                 addDesktop ? PairingToken::Kind::AddDesktop : PairingToken::Kind::NewRunner);
    }
    // A token from another encoder: keys in another order, an unknown field.
    const QByteArray json = R"({"r":null,"kv":1,"x":"later","fp":")" +
                            QByteArray(v0.desktopFingerprint) +
                            R"(","m":"m1","w":"w1","s":"s1","t":"t1"})";
    const auto foreign = PairingToken::parse(tokenOf(json));
    QVERIFY(foreign.ok());
    QCOMPARE(foreign.value().fields().ticketId, QStringLiteral("t1"));
    QCOMPARE(foreign.value().kind(), PairingToken::Kind::NewRunner);
}

void RunnerPairingTest::tokenRejects_data() {
    QTest::addColumn<QByteArray>("token");
    QTest::addColumn<PairingError>("error");
    const QByteArray fp = v0.desktopFingerprint;
    const auto with = [&](const QByteArray& replace, const QByteArray& by) {
        QByteArray json = R"({"t":"t1","s":")" + QByteArray(secret) +
                          R"(","w":"w1","m":"m1","kv":1,"fp":")" + fp + R"(","r":null})";
        json.replace(replace, by);
        return tokenOf(json);
    };
    QTest::newRow("valid") << with("", "") << PairingError::InvalidKey; // sentinel: parses
    QTest::newRow("prefix kpr2") << QByteArray("kpr2." + with("", "").mid(5))
                                 << PairingError::TokenPrefix;
    QTest::newRow("no prefix") << with("", "").mid(5) << PairingError::TokenPrefix;
    QTest::newRow("bad base64url") << QByteArray("kpr1.ab+c/d==") << PairingError::TokenEncoding;
    QTest::newRow("base64 padding")
        << QByteArray(with("", "") + "=") << PairingError::TokenEncoding;
    QTest::newRow("not json") << tokenOf("not json") << PairingError::TokenEncoding;
    QTest::newRow("array") << tokenOf("[1,2]") << PairingError::TokenNotObject;
    QTest::newRow("string") << tokenOf("\"x\"") << PairingError::TokenNotObject;
    QTest::newRow("number") << tokenOf("42") << PairingError::TokenNotObject;
    QTest::newRow("two values") << tokenOf("1,2") << PairingError::TokenEncoding;
    for (const char* field : {"t", "s", "w", "m", "kv", "fp", "r"}) {
        QTest::addRow("missing %s", field)
            << with(QByteArray("\"") + field + "\":", QByteArray("\"_") + field + "\":")
            << PairingError::TokenMissingField;
    }
    QTest::newRow("t number") << with(R"("t":"t1")", R"("t":5)") << PairingError::TokenFieldType;
    QTest::newRow("t empty") << with(R"("t":"t1")", R"("t":"")") << PairingError::TokenFieldType;
    QTest::newRow("s null") << with(QByteArray(R"("s":")") + secret + "\"", R"("s":null)")
                            << PairingError::TokenFieldType;
    QTest::newRow("w array") << with(R"("w":"w1")", R"("w":[])") << PairingError::TokenFieldType;
    QTest::newRow("m object") << with(R"("m":"m1")", R"("m":{})") << PairingError::TokenFieldType;
    QTest::newRow("kv string") << with(R"("kv":1)", R"("kv":"1")") << PairingError::TokenFieldType;
    QTest::newRow("kv zero") << with(R"("kv":1)", R"("kv":0)") << PairingError::TokenFieldType;
    QTest::newRow("kv fraction") << with(R"("kv":1)", R"("kv":1.5)")
                                 << PairingError::TokenFieldType;
    QTest::newRow("kv huge") << with(R"("kv":1)", R"("kv":1e12)") << PairingError::TokenFieldType;
    QTest::newRow("fp short") << with(fp, "AAAA") << PairingError::TokenFieldType;
    QTest::newRow("fp not base64url")
        << with(fp, fp.left(42) + "=") << PairingError::TokenFieldType;
    QTest::newRow("r number") << with(R"("r":null)", R"("r":1)") << PairingError::TokenFieldType;
    QTest::newRow("r empty") << with(R"("r":null)", R"("r":"")") << PairingError::TokenFieldType;
    QTest::newRow("too long") << with("\"t1\"", "\"" + QByteArray(2000, 't') + "\"")
                              << PairingError::TokenTooLong;
}

void RunnerPairingTest::tokenRejects() {
    QFETCH(QByteArray, token);
    QFETCH(PairingError, error);
    const auto parsed = PairingToken::parse(token);
    if (error == PairingError::InvalidKey) {
        QVERIFY(parsed.ok());
        return;
    }
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error(), error);
}

void RunnerPairingTest::tokenNeverPrinted() {
    const auto token = PairingToken::create(tokenFields(v0, true));
    QVERIFY(token.ok());
    const QString printed = debugText(token.value());
    const QByteArray encoded = token.value().encode();
    for (const QString& field :
         {str(secret), str(v0.ticketId), str(v0.runnerId), str(v0.desktopFingerprint),
          QString::fromLatin1(encoded.mid(5, 20))})
        QVERIFY2(!printed.contains(field), qPrintable(printed));
    QVERIFY(printed.contains(QStringLiteral("redacted")));
    // Every error a token can produce names no part of it.
    const QByteArray damaged = encoded.left(encoded.size() - 3) + "!!!";
    for (const QByteArray& bad :
         {damaged, QByteArray("kpr9.") + encoded.mid(5), encoded + QByteArray(3000, 'A')}) {
        const auto parsed = PairingToken::parse(bad);
        QVERIFY(!parsed.ok());
        const QString code = errorCode(parsed.error());
        QVERIFY(!code.contains(str(secret)));
        QVERIFY(!code.contains(QString::fromLatin1(encoded.mid(5, 8))));
        QVERIFY(QRegularExpression(QStringLiteral("^[a-z_]+$")).match(code).hasMatch());
    }
}

void RunnerPairingTest::vectors_data() {
    QTest::addColumn<int>("index");
    for (int i = 0; i < 3; ++i)
        QTest::addRow("vector %d", i) << i;
}

void RunnerPairingTest::vectors() {
    QFETCH(int, index);
    const Vector& v = kVectors[index];
    QCOMPARE(runnerFingerprint(hex(v.runnerPublicKey)), str(v.runnerFingerprint));
    QCOMPARE(desktopFingerprint(hex(v.desktopSigningPublicKey), hex(v.desktopAgreementPublicKey)),
             str(v.desktopFingerprint));
    QCOMPARE(pairingDigestBytes(digestInput(v)), QByteArray(v.digestInput));
    const PairingCode code = pairingCode(digestInput(v));
    QCOMPARE(code.digest, str(v.digest));
    QCOMPARE(code.code, str(v.code));
    QVERIFY(QRegularExpression(QStringLiteral("^[A-Z2-7]{5}-[A-Z2-7]{5}$"))
                .match(code.code)
                .hasMatch());
    QVERIFY(runnerFingerprint(hex(v.runnerPublicKey).left(31)).isEmpty());
    QVERIFY(desktopFingerprint(hex(v.desktopSigningPublicKey), QByteArray()).isEmpty());
}

void RunnerPairingTest::digestChangesWithEachField_data() {
    QTest::addColumn<DigestChange>("change");
    for (const auto& [field, change] : digestChanges)
        QTest::newRow(field) << change;
}

void RunnerPairingTest::digestChangesWithEachField() {
    QFETCH(DigestChange, change);
    const PairingCode base = pairingCode(digestInput(v0));
    PairingDigestInput input = digestInput(v0);
    change(input);
    const PairingCode changed = pairingCode(input);
    QVERIFY(changed.digest != base.digest);
    QVERIFY(changed.code != base.code);
}

void RunnerPairingTest::desktopKeyCheck_data() {
    QTest::addColumn<std::function<void(ReportedDesktop&)>>("change");
    QTest::addColumn<bool>("matches");
    using Change = std::function<void(ReportedDesktop&)>;
    QTest::newRow("as in token") << Change([](auto&) {}) << true;
    QTest::newRow("other signing key")
        << Change([](auto& d) { d.signingPublicKey = hex(kVectors[1].desktopSigningPublicKey); })
        << false;
    QTest::newRow("other agreement key") << Change([](auto& d) {
        d.agreementPublicKey = hex(kVectors[1].desktopAgreementPublicKey);
    }) << false;
    QTest::newRow("keys swapped") << Change([](auto& d) {
        std::swap(d.signingPublicKey, d.agreementPublicKey);
    }) << false;
    QTest::newRow("short key") << Change([](auto& d) { d.signingPublicKey.chop(1); }) << false;
    QTest::newRow("other machine id")
        << Change([](auto& d) { d.machineId = QStringLiteral("m_other"); }) << false;
    QTest::newRow("other key version") << Change([](auto& d) { ++d.machineKeyVersion; }) << false;
}

void RunnerPairingTest::desktopKeyCheck() {
    QFETCH(std::function<void(ReportedDesktop&)>, change);
    QFETCH(bool, matches);
    const auto token = PairingToken::create(tokenFields(v0, false));
    QVERIFY(token.ok());
    ReportedDesktop desktop{str(v0.machineId), v0.machineKeyVersion,
                            hex(v0.desktopSigningPublicKey), hex(v0.desktopAgreementPublicKey)};
    change(desktop);
    const auto error = checkDesktopKeys(token.value(), desktop);
    if (matches) {
        QVERIFY(!error);
        return;
    }
    QVERIFY(error);
    QCOMPARE(*error, PairingError::DesktopKeyMismatch);
    QCOMPARE(errorCode(*error), QStringLiteral("desktop_key_mismatch"));
}

void RunnerPairingTest::signaturesMatchVectors() {
    const QByteArray runnerSeed = hex(v0.runnerSeed);
    const QByteArray desktopSeed = hex(v0.desktopSeed);
    const struct {
        QByteArray bytes;
        const char* expected;
        const QByteArray& seed;
        const char* signature;
    } cases[] = {
        {registerPayload().bytes(), registerBytes, runnerSeed, registerSignature},
        {approvalPayload().bytes(), approvalBytes, desktopSeed, approvalSignature},
        {challengePayload().bytes(), challengeBytes, runnerSeed, challengeSignature},
        {offerPayload().bytes(), offerBytes, runnerSeed, offerSignature},
        {answerPayload().bytes(), answerBytes, desktopSeed, answerSignature},
    };
    for (const auto& c : cases) {
        QCOMPARE(c.bytes, QByteArray(c.expected));
        // Ed25519 is deterministic: the same signature as the OpenSSL command line.
        QCOMPARE(signPayload(c.seed, c.bytes), sig(c.signature));
    }
    QCOMPARE(sdpDigest(str(offerSdp)),
             QStringLiteral("4c1b50f0e8a0a87e3f8b65de77023b97e3d974085dfb690332f1f839b34b0fbc"));
    QCOMPARE(issuedAtString(at(issuedAt)), str(issuedAt));
    QVERIFY(signPayload(runnerSeed.left(31), QByteArray("x")).isEmpty());
}

void RunnerPairingTest::signaturesVerify() {
    const QDateTime now = at(issuedAt);
    QVERIFY(!verify(registerPayload(), sig(registerSignature), runnerKey(), now));
    QVERIFY(!verifyApproval(pendingPin(), sig(approvalSignature)));
    QVERIFY(!verifyApproval(pendingPin(), sig(approvalSignature), v0.machineKeyVersion));
    QVERIFY(!verify(challengePayload(), sig(challengeSignature), runnerKey(), now));
    QVERIFY(!verify(offerPayload(), sig(offerSignature), runnerKey(), now));
    QVERIFY(!verify(answerPayload(), sig(answerSignature), desktopKey(), now));

    // Freshly generated keys sign and verify too.
    const QByteArray seed = hex(kVectors[1].runnerSeed);
    const VerifyingKey key{hex(kVectors[1].runnerPublicKey), 1};
    ChallengePayload challenge = challengePayload();
    challenge.issuedAt = issuedAtString(QDateTime::currentDateTimeUtc());
    QVERIFY(!verify(challenge, signPayload(seed, challenge.bytes()), key,
                    QDateTime::currentDateTimeUtc()));
}

namespace {
template <typename Payload, typename Verify>
void expectEveryChangeFails(
    const char* type, const Payload& base, const QByteArray& signature,
    const std::vector<std::pair<const char*, std::function<void(Payload&)>>>& changes,
    Verify verifyChanged) {
    for (const auto& [field, change] : changes) {
        Payload changed = base;
        change(changed);
        const auto error = verifyChanged(changed, signature);
        QVERIFY2(error.has_value(), qPrintable(QStringLiteral("%1.%2").arg(
                                        QLatin1StringView(type), QLatin1StringView(field))));
    }
}
} // namespace

void RunnerPairingTest::signatureFieldChangesFail() {
    const QDateTime now = at(issuedAt);
    const auto x = [](QString& s) { s += QLatin1Char('x'); };
    // issuedAt moves by one millisecond, so it stays inside the window.
    const auto later = [](QString& s) { s = str("2026-11-18T03:00:00.001Z"); };

    expectEveryChangeFails<RegisterPayload>(
        "register", registerPayload(), sig(registerSignature),
        {{"ticketId", [&](auto& p) { x(p.ticketId); }},
         {"workspaceId", [&](auto& p) { x(p.workspaceId); }},
         {"machineId", [&](auto& p) { x(p.machineId); }},
         {"runnerFingerprint",
          [&](auto& p) { p.runnerFingerprint = str(kVectors[1].runnerFingerprint); }},
         {"issuedAt", [&](auto& p) { later(p.issuedAt); }}},
        [&](const auto& p, const auto& s) { return verify(p, s, runnerKey(), now); });
    // The key version is signed: a payload naming version 2 fails against a version 2 pin.
    RegisterPayload version = registerPayload();
    version.runnerKeyVersion = 2;
    QCOMPARE(verify(version, sig(registerSignature), {hex(v0.runnerPublicKey), 2}, now),
             PairingError::InvalidSignature);

    expectEveryChangeFails<ChallengePayload>(
        "challenge", challengePayload(), sig(challengeSignature),
        {{"runnerId", [&](auto& p) { x(p.runnerId); }},
         {"nonce", [&](auto& p) { x(p.nonce); }},
         {"issuedAt", [&](auto& p) { later(p.issuedAt); }}},
        [&](const auto& p, const auto& s) { return verify(p, s, runnerKey(), now); });
    ChallengePayload challengeVersion = challengePayload();
    challengeVersion.runnerKeyVersion = 2;
    QCOMPARE(verify(challengeVersion, sig(challengeSignature), {hex(v0.runnerPublicKey), 2}, now),
             PairingError::InvalidSignature);

    expectEveryChangeFails<OfferPayload>(
        "offer", offerPayload(), sig(offerSignature),
        {{"connectionId", [&](auto& p) { x(p.connectionId); }},
         {"workspaceId", [&](auto& p) { x(p.workspaceId); }},
         {"machineId", [&](auto& p) { x(p.machineId); }},
         {"runnerId", [&](auto& p) { x(p.runnerId); }},
         {"jobId", [&](auto& p) { x(p.jobId); }},
         {"attempt", [&](auto& p) { ++p.attempt; }},
         {"issuedAt", [&](auto& p) { later(p.issuedAt); }},
         {"sdp", [&](auto& p) { x(p.sdp); }}},
        [&](const auto& p, const auto& s) { return verify(p, s, runnerKey(), now); });
    OfferPayload offerVersion = offerPayload();
    offerVersion.runnerKeyVersion = 2;
    QCOMPARE(verify(offerVersion, sig(offerSignature), {hex(v0.runnerPublicKey), 2}, now),
             PairingError::InvalidSignature);

    expectEveryChangeFails<AnswerPayload>(
        "answer", answerPayload(), sig(answerSignature),
        {{"connectionId", [&](auto& p) { x(p.connectionId); }},
         {"workspaceId", [&](auto& p) { x(p.workspaceId); }},
         {"machineId", [&](auto& p) { x(p.machineId); }},
         {"runnerId", [&](auto& p) { x(p.runnerId); }},
         {"jobId", [&](auto& p) { x(p.jobId); }},
         {"offerSdp", [&](auto& p) { x(p.offerSdp); }},
         {"issuedAt", [&](auto& p) { later(p.issuedAt); }},
         {"sdp", [&](auto& p) { x(p.sdp); }}},
        [&](const auto& p, const auto& s) { return verify(p, s, desktopKey(), now); });
    AnswerPayload answerVersion = answerPayload();
    answerVersion.machineKeyVersion = 2;
    QCOMPARE(verify(answerVersion, sig(answerSignature), {hex(v0.desktopSigningPublicKey), 2}, now),
             PairingError::InvalidSignature);

    // A damaged signature.
    QByteArray damaged = sig(offerSignature);
    damaged[0] = static_cast<char>(damaged[0] ^ 1);
    QCOMPARE(verify(offerPayload(), damaged, runnerKey(), now), PairingError::InvalidSignature);
    QCOMPARE(verify(offerPayload(), damaged.left(63), runnerKey(), now),
             PairingError::InvalidSignature);
}

void RunnerPairingTest::signatureWrongKeyOrVersionFails() {
    const QDateTime now = at(issuedAt);
    const VerifyingKey otherRunner{hex(kVectors[1].runnerPublicKey), v0.runnerKeyVersion};
    const VerifyingKey otherDesktop{hex(kVectors[1].desktopSigningPublicKey), v0.machineKeyVersion};
    // Signed by the right party but verified with another key.
    QCOMPARE(verify(registerPayload(), sig(registerSignature), otherRunner, now),
             PairingError::InvalidSignature);
    QCOMPARE(verify(challengePayload(), sig(challengeSignature), otherRunner, now),
             PairingError::InvalidSignature);
    QCOMPARE(verify(offerPayload(), sig(offerSignature), otherRunner, now),
             PairingError::InvalidSignature);
    QCOMPARE(verify(answerPayload(), sig(answerSignature), otherDesktop, now),
             PairingError::InvalidSignature);
    // The desktop key where the runner's belongs, and the reverse.
    QCOMPARE(verify(offerPayload(), sig(offerSignature),
                    {hex(v0.desktopSigningPublicKey), v0.runnerKeyVersion}, now),
             PairingError::InvalidSignature);
    QCOMPARE(verify(answerPayload(), sig(answerSignature),
                    {hex(v0.runnerPublicKey), v0.machineKeyVersion}, now),
             PairingError::InvalidSignature);

    // The pinned key has another version than the payload names.
    const auto bumped = [](VerifyingKey key) {
        ++key.keyVersion;
        return key;
    };
    QCOMPARE(verify(registerPayload(), sig(registerSignature), bumped(runnerKey()), now),
             PairingError::KeyVersionMismatch);
    QCOMPARE(verify(challengePayload(), sig(challengeSignature), bumped(runnerKey()), now),
             PairingError::KeyVersionMismatch);
    QCOMPARE(verify(offerPayload(), sig(offerSignature), bumped(runnerKey()), now),
             PairingError::KeyVersionMismatch);
    QCOMPARE(verify(answerPayload(), sig(answerSignature), bumped(desktopKey()), now),
             PairingError::KeyVersionMismatch);

    // A key of the wrong length.
    QCOMPARE(verify(offerPayload(), sig(offerSignature),
                    {hex(v0.runnerPublicKey).left(31), v0.runnerKeyVersion}, now),
             PairingError::InvalidKey);
    // A register payload whose fingerprint names another key than the one that signed.
    RegisterPayload foreign = registerPayload();
    foreign.runnerFingerprint = str(kVectors[1].runnerFingerprint);
    QCOMPARE(verify(foreign, signPayload(hex(v0.runnerSeed), foreign.bytes()), runnerKey(), now),
             PairingError::InvalidSignature);
}

void RunnerPairingTest::approvalChecksAgainstPendingPin() {
    const QByteArray signature = sig(approvalSignature);
    RunnerPin pin = pendingPin();
    QCOMPARE(pin.pairingDigest, str(v0.digest));
    QVERIFY(!verifyApproval(pin, signature));
    // Read back from its stored form, it still verifies.
    const auto stored = RunnerPin::fromJson(pin.toJson());
    QVERIFY(stored.ok());
    QVERIFY(!verifyApproval(stored.value(), signature));

    // A digest the runner computed from any other input does not verify.
    for (const auto& [field, change] : digestChanges) {
        PairingDigestInput input = digestInput(v0);
        change(input);
        RunnerPin other = pin;
        other.pairingDigest = pairingCode(input).digest;
        QVERIFY2(verifyApproval(other, signature) == PairingError::InvalidSignature, field);
    }
    RunnerPin otherTicket = pin;
    otherTicket.ticketId += QLatin1Char('x');
    QCOMPARE(verifyApproval(otherTicket, signature), PairingError::InvalidSignature);
    // Another desktop key in the pin, or a damaged signature.
    RunnerPin otherKey = pin;
    otherKey.signingPublicKey = hex(kVectors[1].desktopSigningPublicKey);
    QCOMPARE(verifyApproval(otherKey, signature), PairingError::InvalidSignature);
    QByteArray damaged = signature;
    damaged[5] = static_cast<char>(damaged[5] ^ 1);
    QCOMPARE(verifyApproval(pin, damaged), PairingError::InvalidSignature);
    // A key version reported with the approval that is not the pin's.
    QCOMPARE(verifyApproval(pin, signature, v0.machineKeyVersion + 1),
             PairingError::KeyVersionMismatch);
    // Only a pending pin holds what an approval is checked against.
    QCOMPARE(verifyApproval(pin.activated(at(issuedAt)), signature), PairingError::PinInvalid);
    RunnerPin activeWithTicket = pin;
    activeWithTicket.state = RunnerPin::State::Active;
    QCOMPARE(verifyApproval(activeWithTicket, signature), PairingError::PinInvalid);
    RunnerPin shortKey = pin;
    shortKey.signingPublicKey.chop(1);
    QCOMPARE(verifyApproval(shortKey, signature), PairingError::InvalidKey);

    // A pin is only made for the desktop the token names.
    const auto token = PairingToken::create(tokenFields(v0, false));
    ReportedDesktop wrong = reportedDesktop();
    wrong.agreementPublicKey = hex(kVectors[1].desktopAgreementPublicKey);
    const auto refused = RunnerPin::pending(token.value(), wrong, str(v0.digest));
    QVERIFY(!refused.ok());
    QCOMPARE(refused.error(), PairingError::DesktopKeyMismatch);
}

void RunnerPairingTest::issuedAtWindow_data() {
    QTest::addColumn<qint64>("offsetMs");
    QTest::addColumn<bool>("accepted");
    QTest::newRow("same time") << qint64(0) << true;
    QTest::newRow("clock 5 min ahead") << qint64(5 * 60'000) << true;
    QTest::newRow("clock 5 min behind") << qint64(-5 * 60'000) << true;
    QTest::newRow("clock 5 min 1 ms ahead") << qint64(5 * 60'000 + 1) << false;
    QTest::newRow("clock 5 min 1 ms behind") << qint64(-5 * 60'000 - 1) << false;
    QTest::newRow("clock an hour ahead") << qint64(60 * 60'000) << false;
}

void RunnerPairingTest::issuedAtWindow() {
    QFETCH(qint64, offsetMs);
    QFETCH(bool, accepted);
    const QDateTime now = at(issuedAt).addMSecs(offsetMs);
    const auto expect = [&](const std::optional<PairingError>& error) {
        if (accepted)
            QVERIFY(!error);
        else
            QCOMPARE(error, std::optional(PairingError::IssuedAtOutOfWindow));
    };
    expect(verify(registerPayload(), sig(registerSignature), runnerKey(), now));
    expect(verify(challengePayload(), sig(challengeSignature), runnerKey(), now));
    expect(verify(offerPayload(), sig(offerSignature), runnerKey(), now));
    expect(verify(answerPayload(), sig(answerSignature), desktopKey(), now));
    // The approval has no time; it verifies whatever the clock says.
    QVERIFY(!verifyApproval(pendingPin(), sig(approvalSignature)));

    // A time that is not RFC 3339 UTC is refused before the signature is checked.
    for (const char* bad : {"2026-11-18T03:00:00.000+00:00", "2026-11-18 03:00:00Z", "yesterday",
                            "", "2026-13-18T03:00:00.000Z"}) {
        ChallengePayload challenge = challengePayload();
        challenge.issuedAt = str(bad);
        const auto signature = signPayload(hex(v0.runnerSeed), challenge.bytes());
        QCOMPARE(verify(challenge, signature, runnerKey(), at(issuedAt)),
                 std::optional(PairingError::IssuedAtInvalid));
    }
}

void RunnerPairingTest::constantTimeEqualsAndBase64Url() {
    QVERIFY(constantTimeEquals("abc", "abc"));
    QVERIFY(constantTimeEquals({}, {}));
    QVERIFY(!constantTimeEquals("abc", "abd"));
    QVERIFY(!constantTimeEquals("abc", "ab"));
    QCOMPARE(decodeBase64Url(QStringLiteral("AQID")), std::optional(QByteArray("\x01\x02\x03")));
    QCOMPARE(decodeBase64Url(QStringLiteral("_-8")), std::optional(QByteArray("\xff\xef")));
    for (const char* bad : {"AQ==", "AQI=", "A", "AQ+D", "AQ/D", "AQ D", "AR"})
        QVERIFY2(!decodeBase64Url(str(bad)), bad); // "AR": unused bits set
}

void RunnerPairingTest::pinsRoundTrip() {
    const auto token = PairingToken::create(tokenFields(v0, false));
    const RunnerPin pending = pendingPin();
    const RunnerPin active = pending.activated(at(issuedAt));
    for (const RunnerPin& runner : {pending, active}) {
        QJsonObject stored = runner.toJson();
        // A store's own field is ignored.
        stored.insert(QStringLiteral("storeNote"), QStringLiteral("kept by the caller"));
        const QByteArray text = QJsonDocument(stored).toJson();
        // No pin holds the ticket secret or the token.
        QVERIFY(!text.contains(secret));
        QVERIFY(!text.contains(token.value().encode().mid(5, 16)));
        const auto read = RunnerPin::fromJson(QJsonDocument::fromJson(text).object());
        QVERIFY(read.ok());
        QCOMPARE(read.value().machineId, str(v0.machineId));
        QCOMPARE(read.value().workspaceId, str(v0.workspaceId));
        QCOMPARE(read.value().machineKeyVersion, v0.machineKeyVersion);
        QCOMPARE(read.value().signingPublicKey, hex(v0.desktopSigningPublicKey));
        QCOMPARE(read.value().agreementPublicKey, hex(v0.desktopAgreementPublicKey));
        QCOMPARE(read.value().desktopFingerprint, str(v0.desktopFingerprint));
        QCOMPARE(read.value().state, runner.state);
        QCOMPARE(read.value().ticketId, runner.ticketId);
        QCOMPARE(read.value().pairingDigest, runner.pairingDigest);
        QCOMPARE(read.value().pairedAt, runner.pairedAt);
    }
    QCOMPARE(pending.state, RunnerPin::State::Pending);
    QCOMPARE(pending.ticketId, str(v0.ticketId));
    QVERIFY(!pending.pairedAt);
    QVERIFY(pending.toJson().value(QStringLiteral("pairedAt")).isNull());
    QCOMPARE(active.state, RunnerPin::State::Active);
    QVERIFY(!active.toJson().contains(QStringLiteral("ticketId")));
    QVERIFY(!active.toJson().contains(QStringLiteral("pairingDigest")));
    QCOMPARE(active.pairedAt, std::optional(at(issuedAt)));

    DesktopRunnerPin desktop;
    desktop.runnerId = str(v0.runnerId);
    desktop.runnerKeyVersion = v0.runnerKeyVersion;
    desktop.publicKey = hex(v0.runnerPublicKey);
    desktop.runnerFingerprint = str(v0.runnerFingerprint);
    desktop.name = QStringLiteral("build-01 \xc3\xa9");
    desktop.pairedAt = at(issuedAt);
    const auto read = DesktopRunnerPin::fromJson(
        QJsonDocument::fromJson(QJsonDocument(desktop.toJson()).toJson()).object());
    QVERIFY(read.ok());
    QCOMPARE(read.value().runnerId, desktop.runnerId);
    QCOMPARE(read.value().runnerKeyVersion, desktop.runnerKeyVersion);
    QCOMPARE(read.value().publicKey, desktop.publicKey);
    QCOMPARE(read.value().runnerFingerprint, desktop.runnerFingerprint);
    QCOMPARE(read.value().name, desktop.name);
    QCOMPARE(read.value().pairedAt, desktop.pairedAt);
}

namespace {
enum class PinKind { RunnerActive, RunnerPending, Desktop };
}

void RunnerPairingTest::corruptPinsFail_data() {
    QTest::addColumn<PinKind>("kind");
    QTest::addColumn<QByteArray>("field");
    QTest::addColumn<QJsonValue>("value"); // Undefined removes the field
    const QJsonValue gone(QJsonValue::Undefined);
    const auto active = PinKind::RunnerActive;
    const auto pending = PinKind::RunnerPending;
    for (const char* field :
         {"version", "machineId", "workspaceId", "machineKeyVersion", "signingPublicKey",
          "agreementPublicKey", "desktopFingerprint", "pairedAt", "state"})
        QTest::addRow("runner pin without %s", field) << active << QByteArray(field) << gone;
    QTest::newRow("runner pin version 2") << active << QByteArray("version") << QJsonValue(2);
    QTest::newRow("runner pin version 0") << active << QByteArray("version") << QJsonValue(0);
    QTest::newRow("runner pin key version 0")
        << active << QByteArray("machineKeyVersion") << QJsonValue(0);
    QTest::newRow("runner pin key version text")
        << active << QByteArray("machineKeyVersion") << QJsonValue(QStringLiteral("1"));
    QTest::newRow("runner pin short key")
        << active << QByteArray("signingPublicKey") << QJsonValue(QStringLiteral("AAAA"));
    QTest::newRow("runner pin key not base64url")
        << active << QByteArray("agreementPublicKey") << QJsonValue(QStringLiteral("!!!"));
    QTest::newRow("runner pin fingerprint of other keys")
        << active << QByteArray("desktopFingerprint")
        << QJsonValue(str(kVectors[1].desktopFingerprint));
    QTest::newRow("runner pin other signing key")
        << active << QByteArray("signingPublicKey")
        << QJsonValue(str(kVectors[1].desktopFingerprint)); // 32 bytes, not the pinned key
    QTest::newRow("runner pin unknown state")
        << active << QByteArray("state") << QJsonValue(QStringLiteral("revoked"));
    QTest::newRow("runner pin local time")
        << active << QByteArray("pairedAt") << QJsonValue(QStringLiteral("2026-11-18T03:00:00"));
    QTest::newRow("active runner pin null time")
        << active << QByteArray("pairedAt") << QJsonValue(QJsonValue::Null);
    for (const char* field : {"ticketId", "pairingDigest", "pairedAt"})
        QTest::addRow("pending runner pin without %s", field)
            << pending << QByteArray(field) << gone;
    QTest::newRow("pending runner pin with a time")
        << pending << QByteArray("pairedAt") << QJsonValue(str(issuedAt));
    QTest::newRow("pending runner pin short digest")
        << pending << QByteArray("pairingDigest") << QJsonValue(QStringLiteral("AAAA"));
    QTest::newRow("pending runner pin empty ticket")
        << pending << QByteArray("ticketId") << QJsonValue(QString());
    for (const char* field : {"version", "runnerId", "runnerKeyVersion", "publicKey",
                              "runnerFingerprint", "name", "pairedAt"})
        QTest::addRow("desktop pin without %s", field)
            << PinKind::Desktop << QByteArray(field) << gone;
    QTest::newRow("desktop pin version 2")
        << PinKind::Desktop << QByteArray("version") << QJsonValue(2);
    QTest::newRow("desktop pin fingerprint of other key")
        << PinKind::Desktop << QByteArray("runnerFingerprint")
        << QJsonValue(str(kVectors[1].runnerFingerprint));
    QTest::newRow("desktop pin key version fraction")
        << PinKind::Desktop << QByteArray("runnerKeyVersion") << QJsonValue(1.5);
    QTest::newRow("desktop pin name number")
        << PinKind::Desktop << QByteArray("name") << QJsonValue(7);
    QTest::newRow("desktop pin bad time")
        << PinKind::Desktop << QByteArray("pairedAt") << QJsonValue(QStringLiteral("soon"));
}

void RunnerPairingTest::corruptPinsFail() {
    QFETCH(PinKind, kind);
    QFETCH(QByteArray, field);
    QFETCH(QJsonValue, value);
    const DesktopRunnerPin desktop{str(v0.runnerId),           v0.runnerKeyVersion,
                                   hex(v0.runnerPublicKey),    str(v0.runnerFingerprint),
                                   QStringLiteral("build-01"), at(issuedAt)};
    QJsonObject json = kind == PinKind::Desktop ? desktop.toJson()
                       : kind == PinKind::RunnerPending
                           ? pendingPin().toJson()
                           : pendingPin().activated(at(issuedAt)).toJson();
    if (value.isUndefined())
        json.remove(QString::fromLatin1(field));
    else
        json.insert(QString::fromLatin1(field), value);
    const PairingError error = kind == PinKind::Desktop ? DesktopRunnerPin::fromJson(json).error()
                                                        : RunnerPin::fromJson(json).error();
    const bool ok = kind == PinKind::Desktop ? DesktopRunnerPin::fromJson(json).ok()
                                             : RunnerPin::fromJson(json).ok();
    QVERIFY(!ok);
    QCOMPARE(error, PairingError::PinInvalid);
    // Empty records.
    QVERIFY(!RunnerPin::fromJson({}).ok());
    QVERIFY(!DesktopRunnerPin::fromJson({}).ok());
}

QTEST_GUILESS_MAIN(RunnerPairingTest)
#include "RunnerPairingTest.moc"
