#include "agent/AgentWorkspaceView.h"
#include "agent/FileReveal.h"
#include "app/MainWindow.h"
#include "app/TranscriptFileType.h"
#include "editor/EditorWidget.h"
#include "ui/Theme.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QFile>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QtTest>

class TranscriptFileOpeningTest final : public QObject {
    Q_OBJECT

  public slots:
    void recordOpen(const QUrl& url) { opened_.append(url); }

  private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        qputenv("KETPLUS_DAEMON_SOCKET", settings_.filePath("isolated.sock").toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", settings_.filePath("isolated-token").toUtf8());
        qputenv("KETPLUS_CONVERSATION_WORKTREES", settings_.filePath("worktrees.sqlite").toUtf8());
    }

    void cleanup() {
        QDesktopServices::unsetUrlHandler("file");
        QDesktopServices::unsetUrlHandler("smb");
        QDesktopServices::unsetUrlHandler("zoommtg");
        ketplus::file_reveal::setHandler({});
    }
    void choosesLinkActions() {
        using ketplus::TranscriptFileAction;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDir root(directory.path());
        const auto write = [&root](const QString& name, const QByteArray& bytes,
                                   bool executable = false) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
                return false;
            file.close();
            return !executable || file.setPermissions(file.permissions() | QFileDevice::ExeOwner);
        };
        QVERIFY(write("image.png", QByteArray::fromHex("89504e470d0a1a0a0000")));
        QVERIFY(write("report.pdf", "%PDF-1.7\n"));
        QVERIFY(write("clip.mp4", QByteArray::fromHex("000000186674797069736f6d")));
        QVERIFY(write("run.sh", "#!/bin/sh\necho hello\n", true));
        QVERIFY(write("report", QByteArray::fromHex("cffaedfe07000001"), true));
        QVERIFY(write("tool.jar", QByteArray::fromHex("504b0304000000")));
        QVERIFY(write("Shell.terminal", QByteArray("bplist00\x01\x02", 10)));
        QVERIFY(root.mkpath("Example.app/Contents"));
        QVERIFY(root.mkpath("folder"));

        const auto action = [&root](const QString& name) {
            return ketplus::transcriptFileAction(root.filePath(name));
        };
        QCOMPARE(action("image.png"), TranscriptFileAction::View);
        QCOMPARE(action("report.pdf"), TranscriptFileAction::View);
        QCOMPARE(action("clip.mp4"), TranscriptFileAction::View);
        // A script opens as text; nothing runs.
        QCOMPARE(action("run.sh"), TranscriptFileAction::Edit);
        for (const QString& name : {QStringLiteral("report"), QStringLiteral("tool.jar"),
                                    QStringLiteral("Shell.terminal"), QStringLiteral("Example.app"),
                                    QStringLiteral("folder")})
            QVERIFY2(action(name) == TranscriptFileAction::Reveal, qPrintable(name));
        QCOMPARE(action("missing.png"), TranscriptFileAction::Missing);

        using ketplus::TranscriptUrlAction;
        QCOMPARE(ketplus::transcriptUrlAction(QUrl("https://example.com")),
                 TranscriptUrlAction::Open);
        QCOMPARE(ketplus::transcriptUrlAction(QUrl("mailto:team@example.com")),
                 TranscriptUrlAction::Open);
        for (const char* link : {"smb://attacker.example/share", "ssh://host", "zoommtg:join",
                                 "ms-msdt:/id", "vscode://file/x"})
            QVERIFY2(ketplus::transcriptUrlAction(QUrl(link)) == TranscriptUrlAction::Confirm,
                     link);
        QCOMPARE(ketplus::transcriptUrlAction(QUrl("file://attacker.example/share/x")),
                 TranscriptUrlAction::Refuse);
        QVERIFY(ketplus::isRemoteFileLink("//attacker.example/share/x"));
        QVERIFY(ketplus::isRemoteFileLink("\\\\attacker.example\\share"));
        QVERIFY(ketplus::isRemoteFileLink("file://attacker.example/share/x"));
        QVERIFY(!ketplus::isRemoteFileLink("file:///tmp/x"));
        QVERIFY(!ketplus::isRemoteFileLink("file://localhost/tmp/x"));
        QVERIFY(!ketplus::isRemoteFileLink("/tmp/x"));
    }

    void classifiesFiles_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QByteArray>("contents");
        QTest::addColumn<bool>("text");
        QTest::newRow("source") << QStringLiteral("main.cpp")
                                << QByteArray("int main() { return 0; }\n") << true;
        QTest::newRow("typescript") << QStringLiteral("main.ts")
                                    << QByteArray("export const answer: number = 42;\n") << true;
        QTest::newRow("javascript-module")
            << QStringLiteral("brand.mjs") << QByteArray("export const brand = 'KetPlus';\n")
            << true;
        QTest::newRow("json") << QStringLiteral("config.json") << QByteArray("{\"enabled\":true}\n")
                              << true;
        QTest::newRow("xml") << QStringLiteral("config.xml")
                             << QByteArray("<?xml version=\"1.0\"?><root/>") << true;
        QTest::newRow("unicode") << QStringLiteral("notes.md")
                                 << QStringLiteral("Tiếng Việt 😌\n").toUtf8() << true;
        QTest::newRow("extensionless")
            << QStringLiteral("README") << QByteArray("Project notes\n") << true;
        QTest::newRow("unknown-text")
            << QStringLiteral("source.custom") << QByteArray("custom source\n") << true;
        QTest::newRow("empty") << QStringLiteral("new.txt") << QByteArray() << true;
        QTest::newRow("utf16") << QStringLiteral("notes.txt")
                               << QByteArray::fromHex("fffe680069000a00") << true;
        QTest::newRow("script") << QStringLiteral("run.sh") << QByteArray("#!/bin/sh\necho hello\n")
                                << true;
        QTest::newRow("png") << QStringLiteral("image.png")
                             << QByteArray::fromHex("89504e470d0a1a0a0000") << false;
        QTest::newRow("svg") << QStringLiteral("vector.svg")
                             << QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\"/>") << false;
        QTest::newRow("pdf") << QStringLiteral("report.pdf") << QByteArray("%PDF-1.7\n") << false;
        QTest::newRow("zip") << QStringLiteral("archive.zip")
                             << QByteArray::fromHex("504b0304000000") << false;
        QTest::newRow("office") << QStringLiteral("report.docx")
                                << QByteArray::fromHex("504b0304000000") << false;
        QTest::newRow("binary-txt")
            << QStringLiteral("wrong.txt") << QByteArray::fromHex("00010203ff") << false;
        QTest::newRow("pdf-txt") << QStringLiteral("wrong.txt") << QByteArray("%PDF-1.7\n")
                                 << false;
        QTest::newRow("no-extension-binary")
            << QStringLiteral("binary") << QByteArray::fromHex("cffaedfe00000000") << false;
        QTest::newRow("empty-pdf") << QStringLiteral("empty.pdf") << QByteArray() << false;
    }

    void classifiesFiles() {
        QFETCH(QString, name);
        QFETCH(QByteArray, contents);
        QFETCH(bool, text);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(name);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), contents.size());
        file.close();
        QCOMPARE(ketplus::isTranscriptTextFile(path), text);
    }

    void routesTranscriptLinks() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDir root(QFileInfo(directory.path()).canonicalFilePath());
        const auto write = [&root](const QString& name, const QByteArray& bytes) {
            QFile file(root.filePath(name));
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        QVERIFY(write("notes.txt", "one\ntwo\nthree\n"));
        QVERIFY(write("ảnh #1.png", QByteArray::fromHex("89504e470d0a1a0a0000")));
        QVERIFY(write("report.pdf", "%PDF-1.7\n"));
        QVERIFY(root.mkpath("Example.app/Contents"));
        QVERIFY(QFile::link(root.filePath("ảnh #1.png"), root.filePath("image-link")));
        QVERIFY(write("report", QByteArray::fromHex("cffaedfe07000001")));
        QVERIFY(QFile(root.filePath("report"))
                    .setPermissions(QFile(root.filePath("report")).permissions() |
                                    QFileDevice::ExeOwner));
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        const QString outsideImage =
            QDir(QFileInfo(outside.path()).canonicalFilePath()).filePath("elsewhere.png");
        {
            QFile file(outsideImage);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray::fromHex("89504e470d0a1a0a0000"));
        }

        // Intercept the OS handoff: tests must never launch a viewer or executable.
        opened_.clear();
        QDesktopServices::setUrlHandler("file", this, "recordOpen");
        QDesktopServices::setUrlHandler("smb", this, "recordOpen");
        QDesktopServices::setUrlHandler("zoommtg", this, "recordOpen");
        QStringList revealed;
        ketplus::file_reveal::setHandler(
            [&revealed](const QString& path) { revealed.append(path); });
        // Answers the next confirmation the window asks.
        const auto answerNextQuestion = [](QMessageBox::StandardButton button) {
            QTimer::singleShot(0, [button] {
                if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                    box->button(button)->click();
            });
        };
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.resize(1100, 700);
        window.openFolder(directory.path());
        window.show();
        for (auto* action : window.findChildren<QAction*>()) {
            if (action->text() == "Agent Workspace")
                action->setChecked(true);
        }
        auto* agent = window.findChild<ketplus::AgentWorkspaceView*>();
        QVERIFY(agent);
        agent->openRelativeFile(root.filePath("notes.txt") + ":2");
        ketplus::EditorWidget* textEditor = nullptr;
        for (auto* editor : window.findChildren<ketplus::EditorWidget*>()) {
            if (editor->document().filePath() == root.filePath("notes.txt"))
                textEditor = editor;
        }
        QVERIFY(textEditor);
        QCOMPARE(textEditor->currentLine(), 2);
        QCOMPARE(opened_.size(), 0);
        const auto editorCount = window.findChildren<ketplus::EditorWidget*>().size();

        const QStringList names{"ảnh #1.png", "report.pdf", "image-link"};
        for (const auto& name : names) {
            const QString path = root.filePath(name);
            agent->openRelativeFile(QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded));
            QCOMPARE(opened_.last().toLocalFile(), QFileInfo(path).canonicalFilePath());
            QCOMPARE(window.findChildren<ketplus::EditorWidget*>().size(), editorCount);
            QCOMPARE(textEditor->currentLine(), 2);
        }
        QCOMPARE(opened_.size(), names.size());
        // An app bundle and an executable are revealed, not launched.
        for (const QString& name : {QStringLiteral("Example.app"), QStringLiteral("report")}) {
            agent->openRelativeFile(QUrl::fromLocalFile(root.filePath(name)).toString());
            QCOMPARE(revealed.last(), QFileInfo(root.filePath(name)).canonicalFilePath());
        }
        QCOMPARE(opened_.size(), names.size());
        agent->openRelativeFile(root.filePath("missing.png"));
        QCOMPARE(opened_.size(), names.size());
        QVERIFY(window.statusBar()->currentMessage().startsWith("File not found:"));

        // A viewer type outside the worktree asks first.
        answerNextQuestion(QMessageBox::Cancel);
        agent->openRelativeFile(outsideImage);
        QCOMPARE(opened_.size(), names.size());
        answerNextQuestion(QMessageBox::Open);
        agent->openRelativeFile(outsideImage);
        QCOMPARE(opened_.size(), names.size() + 1);
        QCOMPARE(opened_.last().toLocalFile(), outsideImage);

        // Other schemes ask with the full URL; a network file location is refused outright.
        answerNextQuestion(QMessageBox::Cancel);
        agent->openRelativeFile("smb://attacker.example/share");
        answerNextQuestion(QMessageBox::Cancel);
        agent->openRelativeFile("zoommtg:join?confno=1");
        QCOMPARE(opened_.size(), names.size() + 1);
        answerNextQuestion(QMessageBox::Open);
        agent->openRelativeFile("zoommtg:join?confno=1");
        QCOMPARE(opened_.last().scheme(), QStringLiteral("zoommtg"));
        const qsizetype before = opened_.size();
        for (const QString& link : {QStringLiteral("file://attacker.example/share/x.png"),
                                    QStringLiteral("//attacker.example/share/x.png")}) {
            agent->openRelativeFile(link);
            QCOMPARE(opened_.size(), before);
            QVERIFY(window.statusBar()->currentMessage().startsWith("Refused a link"));
        }
        QCOMPARE(revealed.size(), 2);
    }

  private:
    QTemporaryDir settings_;
    QList<QUrl> opened_;
};

QTEST_MAIN(TranscriptFileOpeningTest)
#include "TranscriptFileOpeningTest.moc"
