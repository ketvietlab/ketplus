#include "editor/EditorWidget.h"
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QtTest>
#include <ScintillaMessages.h>

class EditorFileRefreshTest final : public QObject {
    Q_OBJECT
    static void write(const QString& path, const QByteArray& content) {
        QSaveFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(content), content.size());
        QVERIFY(file.commit());
    }
    static constexpr unsigned int message(Scintilla::Message value) {
        return static_cast<unsigned int>(value);
    }
  private slots:
    void reloadsEqualSizeReplacementAndKeepsTheView() {
        QTemporaryDir dir;
        const auto path = dir.filePath("file.txt");
        const QByteArray first = QByteArray("same line\n").repeated(100) + "VERSION 1\n";
        auto second = first;
        second.replace("VERSION 1", "VERSION 2");
        write(path, first);
        const auto stamp = QFileInfo(path).lastModified();
        ketplus::EditorWidget editor;
        editor.resize(700, 250);
        editor.show();
        QVERIFY(editor.loadFile(path).ok);
        editor.selectTextRange(35, 2, 3);
        editor.send(message(Scintilla::Message::SetFirstVisibleLine), 20);
        const auto caret = editor.send(message(Scintilla::Message::GetCurrentPos));
        const auto anchor = editor.send(message(Scintilla::Message::GetAnchor));
        const auto top = editor.send(message(Scintilla::Message::GetFirstVisibleLine));
        write(path, second); // Atomic replacement, as commonly performed by external editors.
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(stamp, QFileDevice::FileModificationTime));
        file.close();
        QVERIFY(editor.refreshFromDisk().ok);
        QCOMPARE(editor.text(), second);
        QCOMPARE(editor.send(message(Scintilla::Message::GetCurrentPos)), caret);
        QCOMPARE(editor.send(message(Scintilla::Message::GetAnchor)), anchor);
        QCOMPARE(editor.send(message(Scintilla::Message::GetFirstVisibleLine)), top);
        QVERIFY(!editor.document().isModified());
    }
    void preservesDirtyTextAndUnchangedUndoHistory() {
        QTemporaryDir dir;
        const auto path = dir.filePath("file.txt");
        write(path, "original");
        ketplus::EditorWidget editor;
        QVERIFY(editor.loadFile(path).ok);
        editor.replaceAllText("my unsaved edits");
        QVERIFY(editor.document().isModified());
        write(path, "external update");
        QVERIFY(editor.refreshFromDisk().ok);
        QCOMPARE(editor.text(), QByteArray("my unsaved edits"));
        QVERIFY(editor.document().isModified());
        editor.undoEdit();
        write(path, "original");
        QVERIFY(!editor.document().isModified());
        QVERIFY(editor.canRedoEdit());
        QVERIFY(editor.refreshFromDisk().ok);
        QVERIFY(editor.canRedoEdit());
        QCOMPARE(editor.text(), QByteArray("original"));
    }
    void aFailedReadKeepsTheLiveDocument() {
        QTemporaryDir dir;
        const auto path = dir.filePath("file.txt");
        write(path, "keep this readable content");
        ketplus::EditorWidget editor;
        QVERIFY(editor.loadFile(path).ok);
        QVERIFY(QFile::remove(path));
        QVERIFY(!editor.refreshFromDisk().ok);
        QCOMPARE(editor.text(), QByteArray("keep this readable content"));
        QCOMPARE(editor.document().filePath(), path);
        QVERIFY(!editor.document().isModified());
    }
    void keepsTheSelectedEncoding() {
        QTemporaryDir dir;
        const auto path = dir.filePath("latin1.txt");
        write(path, QByteArray("caf\xe9"));
        ketplus::EditorWidget editor;
        QVERIFY(editor.loadFile(path).ok);
        QCOMPARE(editor.document().textEncoding(), ketplus::TextEncoding::Latin1);
        write(path, QByteArray("th\xe9"));
        QVERIFY(editor.refreshFromDisk().ok);
        QCOMPARE(editor.text(), QString::fromUtf8("thé").toUtf8());
        QCOMPARE(editor.document().textEncoding(), ketplus::TextEncoding::Latin1);
    }
};
QTEST_MAIN(EditorFileRefreshTest)
#include "EditorFileRefreshTest.moc"
