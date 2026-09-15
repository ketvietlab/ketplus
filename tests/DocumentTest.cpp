#include "core/Document.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeFile(const QString& path, const QByteArray& content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

} // namespace

class DocumentTest final : public QObject {
    Q_OBJECT

private slots:
    void savesAndLoadsUtf8Content();
    void reportsMissingFiles();
    void detectsAndRoundTripsEncodings_data();
    void detectsAndRoundTripsEncodings();
    void reopensWithChosenEncoding();
    void rejectsCharactersOutsideLatin1();
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
    QCOMPARE(loaded.textEncoding(), ketplus::TextEncoding::Utf8);
}

void DocumentTest::reportsMissingFiles() {
    ketplus::Document document;
    const auto result = document.load(QStringLiteral("/path/that/does/not/exist"));
    QVERIFY(!result.ok);
    QVERIFY(!result.error.isEmpty());
}

void DocumentTest::detectsAndRoundTripsEncodings_data() {
    QTest::addColumn<QByteArray>("raw");
    QTest::addColumn<ketplus::TextEncoding>("encoding");

    QTest::newRow("utf-8 bom") << QByteArray("\xEF\xBB\xBFXin ch\xC3\xA0o\n")
                               << ketplus::TextEncoding::Utf8Bom;
    QTest::newRow("utf-16 le") << QByteArray("\xFF\xFEX\0i\0n\0\n\0", 10)
                               << ketplus::TextEncoding::Utf16LE;
    QTest::newRow("utf-16 be") << QByteArray("\xFE\xFF\0X\0i\0n\0\n", 10)
                               << ketplus::TextEncoding::Utf16BE;
    QTest::newRow("latin-1") << QByteArray("caf\xE9\n") << ketplus::TextEncoding::Latin1;
    QTest::newRow("latin-1 at end") << QByteArray("caf\xE9") << ketplus::TextEncoding::Latin1;
}

void DocumentTest::detectsAndRoundTripsEncodings() {
    QFETCH(QByteArray, raw);
    QFETCH(ketplus::TextEncoding, encoding);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("encoded.txt"));
    QVERIFY(writeFile(path, raw));

    ketplus::Document document;
    const auto loaded = document.load(path);
    QVERIFY2(loaded.ok, qPrintable(loaded.error));
    QCOMPARE(document.textEncoding(), encoding);
    QVERIFY(!loaded.content.startsWith("\xEF\xBB\xBF"));
    QVERIFY(!QString::fromUtf8(loaded.content).contains(QChar::ReplacementCharacter));

    const auto saved = document.save(loaded.content);
    QVERIFY2(saved.ok, qPrintable(saved.error));
    QCOMPARE(readFile(path), raw);
}

void DocumentTest::reopensWithChosenEncoding() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("latin.txt"));
    QVERIFY(writeFile(path, QByteArray("caf\xE9")));

    ketplus::Document document;
    const auto asLatin1 = document.load(path, ketplus::TextEncoding::Latin1);
    QVERIFY(asLatin1.ok);
    QCOMPARE(QString::fromUtf8(asLatin1.content), QStringLiteral("café"));

    document.setTextEncoding(ketplus::TextEncoding::Utf8);
    QVERIFY(document.save(asLatin1.content).ok);
    QCOMPARE(readFile(path), QByteArray("caf\xC3\xA9"));
    QVERIFY(document.load(path).ok);
    QCOMPARE(document.textEncoding(), ketplus::TextEncoding::Utf8);
}

void DocumentTest::rejectsCharactersOutsideLatin1() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ketplus::Document document;
    document.setTextEncoding(ketplus::TextEncoding::Latin1);
    const auto result =
        document.saveAs(directory.filePath(QStringLiteral("x.txt")), QByteArray("Xin ch\xC3\xA0o \xE1\xBB\x9D"));
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("ISO-8859-1")));
}

QTEST_MAIN(DocumentTest)
#include "DocumentTest.moc"
