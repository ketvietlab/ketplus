#include "FakeCodex.h"
#include "FakeCursor.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;

namespace {
QJsonObject capturedRequest(const QString& path, const QByteArray& prefix) {
    QFile log(path);
    if (!log.open(QIODevice::ReadOnly))
        return {};
    QJsonObject result;
    for (const QByteArray& line : log.readAll().split('\n')) {
        if (line.startsWith(prefix))
            result = QJsonDocument::fromJson(line.mid(prefix.size())).object();
    }
    return result;
}

void verifyInstructions(const QString& instructions, const QString& role) {
    QVERIFY(instructions.contains(QStringLiteral("[KetPlus browser policy]")));
    const bool expectSkills = role.isEmpty() || role == QStringLiteral("leader");
    for (const auto* skill : {"enterprise-grade", "ket-design-system", "ket-atlas"})
        QCOMPARE(instructions.contains(QLatin1String(skill) + QStringLiteral("/SKILL.md")),
                 expectSkills);
}
} // namespace

class ProviderSkillPolicyTest final : public QObject {
    Q_OBJECT

  private slots:
    void skillInstructions_data() {
        QTest::addColumn<bool>("codex");
        QTest::addColumn<QString>("role");
        for (const bool codex : {true, false}) {
            for (const auto* role : {"", "leader", "developer", "reviewer"}) {
                const QByteArray name =
                    QByteArray(codex ? "codex-" : "acp-") + (*role ? role : "ordinary");
                QTest::newRow(name.constData()) << codex << QString::fromLatin1(role);
            }
        }
    }

    void skillInstructions() {
        QFETCH(bool, codex);
        QFETCH(QString, role);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto executable = directory.filePath(fixtureName(QStringLiteral("provider")));
        const auto log = directory.filePath(QStringLiteral("provider.log"));
        QJsonObject params{{"cwd", directory.path()}};
        if (!role.isEmpty())
            params.insert("teamRole", role);

        if (codex) {
            QVERIFY(writeFakeCodex(executable, log));
            CodexAdapter adapter(executable);
            for (const auto* method : {"thread/start", "thread/resume"}) {
                QFile::remove(log);
                bool done = false;
                QString error;
                adapter.call(QLatin1String(method), params,
                             [&](const QJsonObject& result, const QString& failure) {
                                 error = failure;
                                 params.insert("threadId", result["thread"]["id"]);
                                 done = true;
                             });
                QTRY_VERIFY(done);
                QVERIFY2(error.isEmpty(), qPrintable(error));
                const auto sent = capturedRequest(log, "setup:");
                QVERIFY(!sent.isEmpty());
                QVERIFY(!sent.contains("teamRole"));
                verifyInstructions(sent["developerInstructions"].toString(), role);
            }
        } else {
            QVERIFY(writeFakeCursor(executable, log));
            CursorAdapter adapter(executable);
            params.insert("sessionId", "session");
            params.insert("prompt", "Inspect the project");
            bool done = false;
            QString error;
            adapter.call("turn/start", params, [&](const QJsonObject&, const QString& failure) {
                error = failure;
                done = true;
            });
            QTRY_VERIFY(done);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QTRY_VERIFY(!capturedRequest(log, "policy:").isEmpty());
            verifyInstructions(capturedRequest(log, "policy:")["instructions"].toString(), role);
        }
    }
};

QTEST_GUILESS_MAIN(ProviderSkillPolicyTest)
#include "ProviderSkillPolicyTest.moc"
