#include "cloud-privacy/MemoryCredentials.h"

#include "cloud/CredentialStore.h"
#include "protocol/Frames.h"
#include "runner/sync/SnapshotKeys.h"

#include <QRegularExpression>
#include <QTest>

using namespace ketplus::daemon::runner::sync;
namespace protocol = ketplus::runner::protocol;

namespace {

QByteArray fixedSecret() {
    QByteArray secret(runnerSecretBytes, Qt::Uninitialized);
    for (int i = 0; i < runnerSecretBytes; ++i)
        secret[i] = static_cast<char>(i);
    return secret;
}

const QString account = QString::fromLatin1(runnerSecretAccount);

QByteArray storedRecord() {
    return r03::credentials.value(QStringLiteral("dev.ketplus.daemon.control/") + account);
}

} // namespace

class SnapshotKeysTest final : public QObject {
    Q_OBJECT

  private slots:
    void init() { r03::resetCredentials(); }

    void keysMatchTheFixedVector() {
        const QByteArray secret = fixedSecret();
        QCOMPARE(repositoryKey(secret, QStringLiteral("runner-a"), QStringLiteral("repo")),
                 QStringLiteral("7X0Mw7Ok-3fwCoIzz2OnD9vVVxnJooK-"));
        QCOMPARE(workspaceKey(secret, QStringLiteral("runner-a"), QStringLiteral("repo"),
                              QStringLiteral("wt")),
                 QStringLiteral("WTqX-UCm9Azol6_YM8-FRbm_J_wZgnOJ"));
    }

    void keysDifferPerRunnerRepositoryAndWorktree() {
        const QByteArray secret = fixedSecret();
        const QString base = workspaceKey(secret, "runner-a", "/r/one/.git", "/r/one");
        QVERIFY(base != workspaceKey(secret, "runner-b", "/r/one/.git", "/r/one"));
        QVERIFY(base != workspaceKey(secret, "runner-a", "/r/two/.git", "/r/one"));
        QVERIFY(base != workspaceKey(secret, "runner-a", "/r/one/.git", "/r/one-linked"));
        QByteArray other = secret;
        other[0] = 9;
        QVERIFY(base != workspaceKey(other, "runner-a", "/r/one/.git", "/r/one"));
        const QString repository = repositoryKey(secret, "runner-a", "/r/one/.git");
        QVERIFY(repository != repositoryKey(secret, "runner-b", "/r/one/.git"));
        QVERIFY(repository != repositoryKey(secret, "runner-a", "/r/two/.git"));
        QCOMPARE(repository, repositoryKey(secret, "runner-a", "/r/one/.git"));
        // Element boundaries are part of the input.
        QVERIFY(workspaceKey(secret, "a", "bc", "d") != workspaceKey(secret, "ab", "c", "d"));
        QVERIFY(workspaceKey(QByteArray(31, 'x'), "a", "b", "c").isEmpty());
    }

    void keysPassTheProtocolFieldRules() {
        const QByteArray secret = fixedSecret();
        static const QRegularExpression form(QStringLiteral("^[A-Za-z0-9_-]{32}$"));
        protocol::Hello hello;
        hello.connectionId = QStringLiteral("c-1");
        hello.capabilities = {QStringLiteral("runner.protocol/1")};
        hello.runnerId = QStringLiteral("runner-a");
        hello.runnerVersion = QStringLiteral("1.0.0");
        hello.os = QStringLiteral("linux");
        hello.arch = QStringLiteral("x86_64");
        hello.jobId = QStringLiteral("job-1");
        for (int i = 0; i < 50; ++i) {
            const QString key = workspaceKey(secret, "runner-a", QString::number(i), "w");
            QVERIFY(form.match(key).hasMatch());
            QVERIFY(form.match(repositoryKey(secret, "runner-a", QString::number(i))).hasMatch());
            hello.snapshots.insert(key, QString(40, QLatin1Char('a')));
        }
        QVERIFY(!protocol::validate(hello, protocol::Direction::RunnerToDesktop).has_value());
    }

    void theSecretIsCreatedOnceAndKept() {
        ketplus::daemon::CredentialStore store;
        const RunnerSecret secrets(store);
        QString error;
        const auto first = secrets.obtain(&error);
        QVERIFY2(first.has_value(), qPrintable(error));
        QCOMPARE(first->size(), runnerSecretBytes);
        QVERIFY(!storedRecord().isEmpty());
        const auto second = secrets.obtain();
        QCOMPARE(second, first);
        QCOMPARE(r03::rewrites, 1);
    }

    void aBrokenSecretIsAnErrorAndNeverReplaced_data() {
        QTest::addColumn<QByteArray>("record");
        QTest::newRow("not json") << QByteArray("garbage");
        QTest::newRow("other version") << QByteArray(R"({"version":2,"secret":"AA"})");
        QTest::newRow("short secret") << QByteArray(R"({"version":1,"secret":"AAEC"})");
        QTest::newRow("no secret") << QByteArray(R"({"version":1})");
        QTest::newRow("padded") << QByteArray(
            R"({"version":1,"secret":"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="})");
    }
    void aBrokenSecretIsAnErrorAndNeverReplaced() {
        QFETCH(QByteArray, record);
        r03::credentials.insert(QStringLiteral("dev.ketplus.daemon.control/") + account, record);
        ketplus::daemon::CredentialStore store;
        QString error;
        QVERIFY(!RunnerSecret(store).obtain(&error).has_value());
        QVERIFY(!error.isEmpty());
        QCOMPARE(storedRecord(), record);
        QCOMPARE(r03::rewrites, 0);
    }

    void anUnreadableOrUnwritableStoreIsAnError() {
        ketplus::daemon::CredentialStore store;
        QString error;
        r03::failReadsOf = account;
        QVERIFY(!RunnerSecret(store).obtain(&error).has_value());
        QVERIFY(!error.isEmpty());
        QVERIFY(storedRecord().isEmpty());
        r03::resetCredentials();
        r03::failWritesTo = account;
        error.clear();
        QVERIFY(!RunnerSecret(store).obtain(&error).has_value());
        QVERIFY(!error.isEmpty());
        QVERIFY(storedRecord().isEmpty());
    }

    void errorsNeverHoldTheSecret() {
        const QByteArray valid =
            R"({"version":1,"secret":"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"})";
        r03::credentials.insert(QStringLiteral("dev.ketplus.daemon.control/") + account,
                                valid.left(valid.size() - 1)); // truncated record
        ketplus::daemon::CredentialStore store;
        QString error;
        QVERIFY(!RunnerSecret(store).obtain(&error).has_value());
        QVERIFY(!error.contains(QStringLiteral("AAECAwQFBgc")));
    }
};

QTEST_GUILESS_MAIN(SnapshotKeysTest)
#include "SnapshotKeysTest.moc"
