#include "core/Document.h"

#include <QTemporaryDir>
#include <QtTest>

class DocumentTest final : public QObject {
    Q_OBJECT

private slots:
    void savesAndLoadsUtf8Content();
    void reportsMissingFiles();
};

void DocumentTest::savesAndLoadsUtf8Content() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("sample.txt"));
    const QByteArray content("Hello, KetPlus!\nXin chào!\n");

    ketplus::Document written;
    const auto saveResult = written.saveAs(path, content);
    QVERIFY2(saveResult.ok, qPrintable(saveResult.error));

    ketplus::Document loaded;
    const auto loadResult = loaded.load(path);
    QVERIFY2(loadResult.ok, qPrintable(loadResult.error));
    QCOMPARE(loadResult.content, content);
    QVERIFY(!loaded.isModified());
}

void DocumentTest::reportsMissingFiles() {
    ketplus::Document document;
    const auto result = document.load(QStringLiteral("/path/that/does/not/exist"));
    QVERIFY(!result.ok);
    QVERIFY(!result.error.isEmpty());
}

QTEST_MAIN(DocumentTest)
#include "DocumentTest.moc"
