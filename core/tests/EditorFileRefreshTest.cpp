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
    void detectsTheEncodingAgainOnRefresh() {
        QTemporaryDir dir;
        const auto path = dir.filePath("rewritten.txt");
        write(path, QByteArray("caf\xe9"));
        ketplus::EditorWidget editor;
        QVERIFY(editor.loadFile(path).ok);
        QCOMPARE(editor.document().textEncoding(), ketplus::TextEncoding::Latin1);
        write(path, QString::fromUtf8("café ✓").toUtf8());
        QVERIFY(editor.refreshFromDisk().ok);
        QCOMPARE(editor.document().textEncoding(), ketplus::TextEncoding::Utf8);
        QCOMPARE(editor.text(), QString::fromUtf8("café ✓").toUtf8());

        // An encoding the user picked survives the next refresh.
        QVERIFY(editor.reloadWithEncoding(ketplus::TextEncoding::Latin1).ok);
        write(path, QString::fromUtf8("thé").toUtf8());
        QVERIFY(editor.refreshFromDisk().ok);
        QCOMPARE(editor.document().textEncoding(), ketplus::TextEncoding::Latin1);
    }
    void keepsEveryByteAfterANul() {
        QTemporaryDir dir;
        const auto path = dir.filePath("data.bin");
        const QByteArray bytes("abc\0def\n", 8);
        write(path, bytes);
        ketplus::EditorWidget editor;
        QVERIFY(editor.loadFile(path).ok);
        QCOMPARE(editor.text(), bytes);
        QVERIFY(!editor.document().isModified());
        QVERIFY(editor.document().saveAs(path, editor.text()).ok);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), bytes);
    }
    void detectsChangesOnDiskSinceLoadOrSave() {
        QTemporaryDir dir;
        const auto path = dir.filePath("file.txt");
        write(path, "original\n");
        ketplus::EditorWidget editor;
        QVERIFY(editor.loadFile(path).ok);
        QVERIFY(!editor.document().changedOnDisk());
        editor.replaceAllText("my edit\n");

        // Same size, same mtime: only the content tells.
        const auto stamp = QFileInfo(path).lastModified();
        write(path, "agent's\n\n");
        QFile touched(path);
        QVERIFY(touched.open(QIODevice::ReadWrite));
        QVERIFY(touched.setFileTime(stamp, QFileDevice::FileModificationTime));
        touched.close();
        QVERIFY(editor.document().changedOnDisk());

        // Reload takes the agent's version and is clean again.
        QVERIFY(editor.reloadFromDisk().ok);
        QCOMPARE(editor.text(), QByteArray("agent's\n\n"));
        QVERIFY(!editor.document().isModified());
        QVERIFY(!editor.document().changedOnDisk());

        QVERIFY(editor.document().saveAs(path, "saved\n").ok);
        QVERIFY(!editor.document().changedOnDisk());
        write(path, "saved\n"); // Rewritten with the same bytes.
        QVERIFY(!editor.document().changedOnDisk());
        QVERIFY(QFile::remove(path));
        QVERIFY(editor.document().changedOnDisk());
    }
    void aHibernatedTabStaysReadOnlyUntilRestored() {
        QTemporaryDir dir;
        const auto path = dir.filePath("file.txt");
        write(path, "on disk\n");
        ketplus::EditorWidget editor;
        QVERIFY(editor.loadFile(path).ok);
        QVERIFY(editor.hibernate());
        QVERIFY(QFile::remove(path));
        QVERIFY(!editor.restoreFromDisk().ok);
        QVERIFY(editor.isHibernated());
        editor.replaceAllText("typed into the empty tab");
        QVERIFY(editor.text().isEmpty());
        QVERIFY(!editor.document().isModified());

        write(path, "back\n");
        QVERIFY(editor.restoreFromDisk().ok);
        QCOMPARE(editor.text(), QByteArray("back\n"));
        editor.replaceAllText("editable again\n");
        QVERIFY(editor.document().isModified());
    }
};
QTEST_MAIN(EditorFileRefreshTest)
#include "EditorFileRefreshTest.moc"
