#include "ToolLocations.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

namespace tools = ketplus::tool_locations;

namespace {
bool write(const QString& path, const QByteArray& content) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

// An npm global prefix: <root>/codex.cmd, <root>/node.exe and the package under node_modules.
bool makeShimTree(const QString& root, const QByteArray& packageJson,
                  const QString& script = QStringLiteral("bin/codex.js")) {
    return write(QDir(root).filePath(QStringLiteral("codex.cmd")), "@echo off\r\n") &&
           write(QDir(root).filePath(QStringLiteral("node.exe")), "") &&
           write(QDir(root).filePath(QStringLiteral("node_modules/@openai/codex/package.json")),
                 packageJson) &&
           write(QDir(root).filePath(QStringLiteral("node_modules/@openai/codex/") + script),
                 "// entry\n");
}
} // namespace

class ToolLocationsTest final : public QObject {
    Q_OBJECT

  private slots:
    void aPlainExecutableStartsAsItself() {
        const auto command =
            tools::npmCliCommand(QStringLiteral("/usr/local/bin/codex"),
                                 QStringLiteral("@openai/codex"), QStringLiteral("codex"));
        QVERIFY(command);
        QCOMPARE(command->program, QStringLiteral("/usr/local/bin/codex"));
        QVERIFY(command->leadingArguments.isEmpty());
    }

    void aShimBecomesNodeAndThePackageScript() {
        QTemporaryDir root;
        QVERIFY(makeShimTree(root.path(), R"({"bin":{"codex":"bin/codex.js"}})"));
        const auto command =
            tools::npmCliCommand(QDir(root.path()).filePath(QStringLiteral("codex.cmd")),
                                 QStringLiteral("@openai/codex"), QStringLiteral("codex"));
        QVERIFY(command);
        QCOMPARE(command->program, QDir(root.path()).filePath(QStringLiteral("node.exe")));
        QCOMPARE(command->leadingArguments,
                 QStringList{QDir::toNativeSeparators(
                     QDir(root.path())
                         .filePath(QStringLiteral("node_modules/@openai/codex/bin/codex.js")))});
    }

    void aStringBinFieldIsAccepted() {
        QTemporaryDir root;
        QVERIFY(makeShimTree(root.path(), R"({"bin":"bin/codex.js"})"));
        QVERIFY(tools::npmCliCommand(QDir(root.path()).filePath(QStringLiteral("codex.cmd")),
                                     QStringLiteral("@openai/codex"), QStringLiteral("codex")));
    }

    void aShimWithoutItsPackageIsNeverStarted() {
        QTemporaryDir root;
        QVERIFY(write(QDir(root.path()).filePath(QStringLiteral("codex.cmd")), "@echo off\r\n"));
        QVERIFY(write(QDir(root.path()).filePath(QStringLiteral("node.exe")), ""));
        QVERIFY(!tools::npmCliCommand(QDir(root.path()).filePath(QStringLiteral("codex.cmd")),
                                      QStringLiteral("@openai/codex"), QStringLiteral("codex")));
    }

    void aMissingBinEntryOrAScriptOutsideThePackageIsRejected() {
        QTemporaryDir other;
        QVERIFY(makeShimTree(other.path(), R"({"bin":{"other":"bin/codex.js"}})"));
        QVERIFY(!tools::npmCliCommand(QDir(other.path()).filePath(QStringLiteral("codex.cmd")),
                                      QStringLiteral("@openai/codex"), QStringLiteral("codex")));

        QTemporaryDir escaping;
        QVERIFY(makeShimTree(escaping.path(), R"({"bin":{"codex":"../../../escape.js"}})"));
        QVERIFY(write(QDir(escaping.path()).filePath(QStringLiteral("escape.js")), "// out\n"));
        QVERIFY(!tools::npmCliCommand(QDir(escaping.path()).filePath(QStringLiteral("codex.cmd")),
                                      QStringLiteral("@openai/codex"), QStringLiteral("codex")));
    }

    void scriptWrappersAreNotPrograms() {
        for (const char* path : {"C:/x/node.cmd", "C:/x/node.BAT", "/x/tool.ps1"})
            QVERIFY(tools::isScriptWrapper(QString::fromLatin1(path)));
        for (const char* path : {"C:/x/node.exe", "/usr/bin/node", "/x/codex"})
            QVERIFY(!tools::isScriptWrapper(QString::fromLatin1(path)));
    }

#if defined(Q_OS_WIN)
    void aNodeCmdOnPathIsNotNode() {
        QTemporaryDir root;
        QVERIFY(makeShimTree(root.path(), R"({"bin":{"codex":"bin/codex.js"}})"));
        QVERIFY(QFile::remove(QDir(root.path()).filePath(QStringLiteral("node.exe"))));
        QTemporaryDir onPath;
        QVERIFY(write(QDir(onPath.path()).filePath(QStringLiteral("node.cmd")), "@echo off\r\n"));
        const QByteArray previousPath = qgetenv("PATH");
        const QByteArray previousProgramFiles = qgetenv("ProgramFiles");
        const QByteArray previousProgramFilesX86 = qgetenv("ProgramFiles(x86)");
        qputenv("PATH", QDir::toNativeSeparators(onPath.path()).toLocal8Bit());
        qputenv("ProgramFiles", QByteArray());
        qputenv("ProgramFiles(x86)", QByteArray());
        const auto command =
            tools::npmCliCommand(QDir(root.path()).filePath(QStringLiteral("codex.cmd")),
                                 QStringLiteral("@openai/codex"), QStringLiteral("codex"));
        qputenv("PATH", previousPath);
        qputenv("ProgramFiles", previousProgramFiles);
        qputenv("ProgramFiles(x86)", previousProgramFilesX86);
        QVERIFY(!command);
    }
#endif

    void findToolSearchesPathThenTheCommonDirectories() {
        QVERIFY(tools::findTool(QStringLiteral("ketplus-no-such-tool-for-test")).isEmpty());
#if !defined(Q_OS_WIN)
        QVERIFY(tools::commonToolDirectories().contains(QStringLiteral("/usr/local/bin")));
        QVERIFY(!tools::findTool(QStringLiteral("sh")).isEmpty());
#endif
    }
};

QTEST_GUILESS_MAIN(ToolLocationsTest)
#include "ToolLocationsTest.moc"
