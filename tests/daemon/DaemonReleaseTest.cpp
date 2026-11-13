#include "ssh/DaemonRelease.h"
#include "ssh/SshDaemonInstaller.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QtTest>
#include <openssl/evp.h>

using namespace ketplus::remote::ssh;
class DaemonReleaseTest final : public QObject {
    Q_OBJECT
    EVP_PKEY* key_{nullptr};
    DaemonReleaseChannel channel_;
    QJsonObject manifest_;
    QByteArray sign(const QByteArray& bytes) {
        auto* context = EVP_MD_CTX_new();
        size_t size = 64;
        QByteArray signature(64, Qt::Uninitialized);
        const bool ok =
            context && EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key_) == 1 &&
            EVP_DigestSign(context, reinterpret_cast<unsigned char*>(signature.data()), &size,
                           reinterpret_cast<const unsigned char*>(bytes.constData()),
                           size_t(bytes.size())) == 1;
        EVP_MD_CTX_free(context);
        return ok ? signature : QByteArray{};
    }
  private slots:
    void initTestCase() {
        auto* context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        QVERIFY(context);
        QVERIFY(EVP_PKEY_keygen_init(context) == 1);
        QVERIFY(EVP_PKEY_keygen(context, &key_) == 1);
        EVP_PKEY_CTX_free(context);
        size_t size = 32;
        QByteArray publicKey(32, Qt::Uninitialized);
        QVERIFY(EVP_PKEY_get_raw_public_key(
                    key_, reinterpret_cast<unsigned char*>(publicKey.data()), &size) == 1);
        channel_ = {QUrl("http://127.0.0.1:8412/manifest.json"), publicKey, "1.2.3",
                    "http://127.0.0.1:9", true};
    }
    void cleanupTestCase() { EVP_PKEY_free(key_); }
    void init() {
        manifest_ = {{"schema", 1},
                     {"product", "ketplus-daemon"},
                     {"version", "1.2.3"},
                     {"tag", "v1.2.3"},
                     {"revision", QString(40, 'a')},
                     {"control_url", "http://127.0.0.1:9"},
                     {"artifacts",
                      QJsonArray{QJsonObject{{"os", "linux"},
                                             {"arch", "x86_64"},
                                             {"layout", "ketplus-runtime-v1"},
                                             {"bytes", 1234},
                                             {"sha256", QString(64, 'b')},
                                             {"url", "http://127.0.0.1:8412/runtime.tar.gz"}}}}};
    }
    void exactSignedBytesSelectMatchingArtifact() {
        const auto bytes = QJsonDocument(manifest_).toJson();
        QString error;
        const auto artifact = verifyDaemonRelease(channel_, bytes, sign(bytes), "x86_64", &error);
        QVERIFY2(artifact, qPrintable(error));
        QCOMPARE(artifact->bytes, 1234);
        QCOMPARE(artifact->remoteDescription().value("revision").toString(), QString(40, 'a'));
        QVERIFY(!verifyDaemonRelease(channel_, bytes + '\n', sign(bytes), "x86_64", &error));
    }
    void rejectsBadSignatureAndUnconfiguredChannel() {
        const auto bytes = QJsonDocument(manifest_).toJson();
        QString error;
        QVERIFY(!verifyDaemonRelease(channel_, bytes, QByteArray(64, 'x'), "x86_64", &error));
        auto unsignedChannel = channel_;
        unsignedChannel.publicKey.clear();
        QVERIFY(!verifyDaemonRelease(unsignedChannel, bytes, sign(bytes), "x86_64", &error));
        QVERIFY(error.contains("no trusted release channel"));
        auto production = DaemonReleaseChannel::production();
        production.publicKey.clear();
        QVERIFY(!production.configured());
        QVERIFY(!production.permits(
            QUrl("http://github.com/ketvietlab/ketplus/releases/download/v1/a")));
        QVERIFY(!production.permits(QUrl("https://github.com.evil.example/a")));
    }
    void rejectsMismatches_data() {
        QTest::addColumn<QString>("field");
        for (const auto* field : {"schema", "product", "version", "tag", "revision", "control_url",
                                  "arch", "duplicate", "bytes", "sha256", "url", "layout"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void rejectsMismatches() {
        QFETCH(QString, field);
        auto artifacts = manifest_.value("artifacts").toArray();
        auto artifact = artifacts.first().toObject();
        if (manifest_.contains(field))
            manifest_[field] = "wrong";
        else if (field == "duplicate")
            artifacts.append(artifact);
        else {
            artifact[field] =
                field == "bytes" ? QJsonValue(daemonArchiveLimit + 1) : QJsonValue("wrong");
            artifacts[0] = artifact;
        }
        manifest_["artifacts"] = artifacts;
        const auto bytes = QJsonDocument(manifest_).toJson();
        QString error;
        QVERIFY(!verifyDaemonRelease(channel_, bytes, sign(bytes), "x86_64", &error));
    }
    void uploadBudgetScalesWithVerifiedArchiveSize() {
        QCOMPARE(daemonInstallTimeoutMs(0), 300000);
        QCOMPARE(daemonInstallTimeoutMs(-1), 300000);
        QCOMPARE(daemonInstallTimeoutMs(1), 301000);
        QCOMPARE(daemonInstallTimeoutMs(267220682), 4378000);
        QCOMPARE(daemonInstallTimeoutMs(daemonArchiveLimit), 8492000);
        QCOMPARE(daemonInstallTimeoutMs(daemonArchiveLimit + 1), 8492000);
    }
    void releasePathsCannotEscapeTheVersionDirectory() {
        auto channel = channel_;
        channel.loopbackQA = false;
        const QByteArray prefix = "https://github.com/ketvietlab/ketplus/releases/download/v1.2.3/";
        QVERIFY(channel.permits(QUrl::fromEncoded(prefix + "daemon.tar.gz")));
        for (const auto* path : {"../daemon.tar.gz", "./daemon.tar.gz", "a/../../daemon.tar.gz",
                                 "%2e%2e/daemon.tar.gz", "a%2f..%2fdaemon.tar.gz"})
            QVERIFY2(!channel.permits(QUrl::fromEncoded(prefix + path)), path);
    }
    void noHostOrKeyNeverStartsAnInstall() {
        SshDaemonInstaller installer(nullptr);
        QSignalSpy finished(&installer, &SshDaemonInstaller::finished);
        installer.review();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().first().toBool(), false);
        QCOMPARE(installer.state(), SshDaemonInstaller::State::Failed);
        installer.confirm(true);
        QCOMPARE(finished.size(), 1);
    }
};
QTEST_GUILESS_MAIN(DaemonReleaseTest)
#include "DaemonReleaseTest.moc"
