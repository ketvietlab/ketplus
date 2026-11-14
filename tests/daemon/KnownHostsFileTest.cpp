#include "ssh/KnownHostsFile.h"

#include <QFile>
#include <QMessageAuthenticationCode>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::remote::ssh;

namespace {

QByteArray hashedName(const QByteArray& name, const QByteArray& salt) {
    return "|1|" + salt.toBase64() + "|" +
           QMessageAuthenticationCode::hash(name, salt, QCryptographicHash::Sha1).toBase64();
}

} // namespace

class KnownHostsFileTest : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        QVERIFY(dir_.isValid());
        file_ = dir_.filePath(QStringLiteral("known_hosts"));
        QFile::remove(file_);
    }

    void removesOnlyThatHost() {
        write("# accepted in ketPlus\n"
              "build-node-02 ssh-ed25519 AAAAone\n"
              "[build-node-02]:2222 ssh-ed25519 AAAAport\n"
              "other ssh-ed25519 AAAAother\n"
              "build-node-02 ecdsa-sha2-nistp256 AAAAtwo\n");
        QVERIFY(hasKnownHost(file_, QStringLiteral("build-node-02")));
        QString error;
        QCOMPARE(removeKnownHost(file_, QStringLiteral("build-node-02"), &error), 2);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(read(), QByteArray("# accepted in ketPlus\n"
                                    "[build-node-02]:2222 ssh-ed25519 AAAAport\n"
                                    "other ssh-ed25519 AAAAother\n"));
        QVERIFY(!hasKnownHost(file_, QStringLiteral("build-node-02")));
        QVERIFY(hasKnownHost(file_, QStringLiteral("[build-node-02]:2222")));
        QCOMPARE(QFile::permissions(file_) & (QFile::ReadGroup | QFile::ReadOther),
                 QFileDevice::Permissions{});
    }

    void keepsOtherNamesOnTheSameLine() {
        write("build-node-02,192.0.2.10 ssh-ed25519 AAAAone\n");
        QCOMPARE(removeKnownHost(file_, QStringLiteral("build-node-02")), 1);
        QCOMPARE(read(), QByteArray("192.0.2.10 ssh-ed25519 AAAAone\n"));
    }

    void removesHashedEntries() {
        const QByteArray line =
            hashedName("build-node-02", "0123456789abcdefghij") + " ssh-ed25519 AAAAhashed\n";
        write(line + hashedName("other", "abcdefghij0123456789") + " ssh-ed25519 AAAAother\n");
        QVERIFY(hasKnownHost(file_, QStringLiteral("build-node-02")));
        QCOMPARE(removeKnownHost(file_, QStringLiteral("build-node-02")), 1);
        QVERIFY(!read().contains("AAAAhashed"));
        QVERIFY(read().contains("AAAAother"));
    }

    void neverRemovesMarkerLines() {
        write("@revoked build-node-02 ssh-ed25519 AAAArevoked\n"
              "@cert-authority build-node-02 ssh-ed25519 AAAAca\n");
        QVERIFY(!hasKnownHost(file_, QStringLiteral("build-node-02")));
        QCOMPARE(removeKnownHost(file_, QStringLiteral("build-node-02")), 0);
        QVERIFY(read().contains("AAAArevoked"));
        QVERIFY(read().contains("AAAAca"));
    }

    void noFileOrNoMatchChangesNothing() {
        QCOMPARE(removeKnownHost(file_, QStringLiteral("build-node-02")), 0);
        QVERIFY(!QFile::exists(file_));
        write("other ssh-ed25519 AAAAother");
        QCOMPARE(removeKnownHost(file_, QStringLiteral("build-node-02")), 0);
        QCOMPARE(read(), QByteArray("other ssh-ed25519 AAAAother"));
        QCOMPARE(removeKnownHost(file_, QString()), 0);
    }

    void lastEntryLeavesAnEmptyFile() {
        write("build-node-02 ssh-ed25519 AAAAone\n");
        QCOMPARE(removeKnownHost(file_, QStringLiteral("build-node-02")), 1);
        QVERIFY(QFile::exists(file_));
        QCOMPARE(read(), QByteArray());
    }

    void aKeySshAppendsDuringTheRewriteIsKept() {
        write("build-node-02 ssh-ed25519 AAAAone\n"
              "other ssh-ed25519 AAAAother\n");
        int attempts = 0;
        const auto sshAppends = [this, &attempts] {
            // The first attempt sees ssh accept a key for another host meanwhile.
            if (attempts++ == 0) {
                QFile out(file_);
                QVERIFY(out.open(QIODevice::Append));
                out.write("new-host ssh-ed25519 AAAAnew\n");
            }
        };
        QString error;
        QCOMPARE(removeKnownHost(file_, QStringLiteral("build-node-02"), &error, sshAppends), 1);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(attempts, 2);
        QCOMPARE(read(), QByteArray("other ssh-ed25519 AAAAother\n"
                                    "new-host ssh-ed25519 AAAAnew\n"));
    }

    void aFileThatKeepsChangingIsLeftAsItIs() {
        write("build-node-02 ssh-ed25519 AAAAone\n");
        int attempts = 0;
        const auto sshKeepsAppending = [this, &attempts] {
            ++attempts;
            QFile out(file_);
            QVERIFY(out.open(QIODevice::Append));
            out.write("busy ssh-ed25519 AAAAbusy\n");
        };
        QString error;
        QCOMPARE(removeKnownHost(file_, QStringLiteral("build-node-02"), &error, sshKeepsAppending),
                 -1);
        QCOMPARE(attempts, 3);
        QVERIFY(!error.isEmpty());
        // Nothing was rewritten: the old key and every appended one are still there.
        QVERIFY(read().startsWith("build-node-02 ssh-ed25519 AAAAone\n"));
        QCOMPARE(read().count("AAAAbusy"), 3);
    }

  private:
    void write(const QByteArray& contents) {
        QFile out(file_);
        QVERIFY(out.open(QIODevice::WriteOnly));
        QCOMPARE(out.write(contents), contents.size());
    }
    QByteArray read() const {
        QFile in(file_);
        return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
    }

    QTemporaryDir dir_;
    QString file_;
};

QTEST_GUILESS_MAIN(KnownHostsFileTest)
#include "KnownHostsFileTest.moc"
