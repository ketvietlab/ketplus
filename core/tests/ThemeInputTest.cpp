#include "ui/theme/io/ThemeInput.h"
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <miniz.h>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

using namespace ket::theme::io;
namespace {
bool write(const QString& path, const QByteArray& data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
QByteArray zip(const QList<QPair<QByteArray, QByteArray>>& entries) {
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_heap(&archive, 0, 0))
        return {};
    for (const auto& entry : entries) {
        if (!mz_zip_writer_add_mem(&archive, entry.first.constData(), entry.second.constData(),
                                   size_t(entry.second.size()), MZ_BEST_COMPRESSION)) {
            mz_zip_writer_end(&archive);
            return {};
        }
    }
    void* buffer = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&archive, &buffer, &size)) {
        mz_zip_writer_end(&archive);
        return {};
    }
    QByteArray result(static_cast<const char*>(buffer), qsizetype(size));
    mz_free(buffer);
    mz_zip_writer_end(&archive);
    return result;
}
qsizetype central(const QByteArray& data) { return data.indexOf(QByteArray("PK\1\2", 4)); }
void put32(QByteArray& data, qsizetype offset, quint32 value) {
    for (int i = 0; i < 4; ++i)
        data[offset + i] = char((value >> (i * 8)) & 255);
}
} // namespace
class ThemeInputTest : public QObject {
    Q_OBJECT
  private slots:
    void jsonc() {
        Error error;
        const auto doc = parseJsonc("{// comment\n"
                                    "\"url\":\"https://example.test/*literal*/\", /* block */"
                                    "\"quote\":\"a\\\"//b\",\"items\":[1,2,/* c */],}",
                                    &error);
        QVERIFY2(!doc.isNull(), qPrintable(error.message));
        QCOMPARE(doc.object()["url"].toString(), QStringLiteral("https://example.test/*literal*/"));
        QCOMPARE(doc.object()["quote"].toString(), QStringLiteral("a\"//b"));
        QVERIFY(parseJsonc("{\n/* unfinished", &error).isNull());
        QCOMPARE(error.line, 2);
        QCOMPARE(error.column, 1);
        QVERIFY(parseJsonc("{\r\n/* unfinished", &error).isNull());
        QCOMPARE(error.line, 2);
        QCOMPARE(error.column, 1);
        QVERIFY(parseJsonc("{\r/* unfinished", &error).isNull());
        QCOMPARE(error.line, 2);
        QVERIFY(parseJsonc("{\n  \"x\": @\n}", &error).isNull());
        QCOMPARE(error.line, 2);
        QVERIFY(error.column >= 8);
        for (auto invalid : {"[,]", "{,}", "[1,,]", "{\"x\":,}", "{\"x\":1 /* no end"})
            QVERIFY2(parseJsonc(invalid, &error).isNull(), invalid);
        QVERIFY(parseJsonc(QByteArray(MaxFileBytes + 1, ' '), &error).isNull());
    }
    void xml() {
        Error error;
        XmlNode root;
        QVERIFY(parseXml("<?xml version=\"1.0\"?><scheme name=\"Test\"><option name=\"x\" "
                         "value=\"&amp;\"/></scheme>",
                         &root, &error));
        QCOMPARE(root.name, QStringLiteral("scheme"));
        QCOMPARE(root.children.first().attributes["value"], QStringLiteral("&"));
        for (auto input :
             {"<!DOCTYPE plist SYSTEM 'https://example.test/test.dtd'><plist/>",
              "<!DOCTYPE a [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><a>&x;</a>",
              "<!DOCTYPE a [<!ENTITY x 'expanded'>]><a>&x;</a>", "<a>&external;</a>", "<a>"}) {
            QVERIFY(!parseXml(input, &root, &error));
            QVERIFY(!error.message.isEmpty());
            QVERIFY(error.line > 0);
            QVERIFY(root.name.isEmpty());
        }
        QByteArray nested;
        for (int i = 0; i < 129; ++i)
            nested += "<a>";
        for (int i = 0; i < 129; ++i)
            nested += "</a>";
        QVERIFY(!parseXml(nested, &root, &error));
    }
    void archives() {
        QTemporaryDir dir;
        const auto path = dir.filePath("test.vsix");
        ZipArchive archive;
        Error error;
        QByteArray data;
        QVERIFY(write(path, zip({{"extension/package.json", "{}"},
                                 {"extension/theme.json", "{\"include\":\"base.json\"}"},
                                 {"extension/base.json", "{}"}})));
        QVERIFY2(archive.open(path, &error), qPrintable(error.message));
        QCOMPARE(archive.files().size(), 3);
        QVERIFY(archive.read("extension/package.json", &data, &error));
        QCOMPARE(data, QByteArray("{}"));
        QVERIFY(!archive.read("missing", &data, &error));
        QVERIFY(data.isEmpty());
        IncludeResolver resolver(archive);
        QVector<JsonSource> chain;
        QVERIFY(resolver.loadJsonChain("extension/theme.json", &chain, &error));
        QCOMPARE(chain.first().path, QStringLiteral("extension/base.json"));
        const QList<QList<QPair<QByteArray, QByteArray>>> unsafe = {{{"../escape", "x"}},
                                                                    {{"/absolute", "x"}},
                                                                    {{"C:/absolute", "x"}},
                                                                    {{"a/../escape", "x"}},
                                                                    {{"a\\b", "x"}},
                                                                    {{"a/./b", "x"}, {"a/b", "y"}},
                                                                    {{"A", "x"}, {"a", "y"}},
                                                                    {{"a", "x"}, {"a/b", "y"}},
                                                                    {{"same", "x"}, {"same", "y"}}};
        for (const auto& entries : unsafe) {
            QVERIFY(write(path, zip(entries)));
            QVERIFY2(!archive.open(path, &error), qPrintable(entries.first().first));
            QVERIFY(!error.message.isEmpty());
            QVERIFY(archive.files().isEmpty());
        }
        QVERIFY(write(
            path, zip({{QByteArray("e\xcc\x81.json"), "x"}, {QByteArray("\xc3\xa9.json"), "y"}})));
        QVERIFY(!archive.open(path, &error));
        auto link = zip({{"link", "target"}});
        put32(link, central(link) + 38, 0120777U << 16U);
        QVERIFY(write(path, link));
        QVERIFY(!archive.open(path, &error));
        QVERIFY(error.message.contains("links"));
        auto bomb = zip({{"bomb", "x"}});
        put32(bomb, central(bomb) + 24, quint32(MaxFileBytes + 1));
        QVERIFY(write(path, bomb));
        QVERIFY(!archive.open(path, &error));
        QVERIFY(error.message.contains("5 MiB"));
        // Dishonest central size must not bypass the bounded output buffer.
        auto dishonest = zip({{"small", QByteArray(8192, 'x')}});
        put32(dishonest, central(dishonest) + 24, 1);
        QVERIFY(write(path, dishonest));
        QVERIFY(archive.open(path, &error));
        QVERIFY(!archive.read("small", &data, &error));
        QVERIFY(data.isEmpty());
        QVERIFY(write(path, zip({{"valid", QByteArray(MaxFileBytes, 'x')}, {"empty", ""}})));
        QVERIFY(archive.open(path, &error));
        QVERIFY(archive.read("valid", &data, &error));
        QCOMPARE(data.size(), MaxFileBytes);
        QVERIFY(archive.read("empty", &data, &error));
        QVERIFY(data.isEmpty());
        auto corrupt = zip({{"bad", "test"}});
        put32(corrupt, central(corrupt) + 16, 1);
        QVERIFY(write(path, corrupt));
        QVERIFY(archive.open(path, &error));
        QVERIFY(!archive.read("bad", &data, &error));
        QVERIFY(data.isEmpty());
    }
    void archiveBudgets() {
        QTemporaryDir dir;
        ZipArchive archive;
        Error error;
        const auto path = dir.filePath("test.jar");
        QList<QPair<QByteArray, QByteArray>> entries;
        for (int i = 0; i <= MaxEntries; ++i)
            entries.append({QByteArray::number(i), ""});
        QVERIFY(write(path, zip(entries)));
        QVERIFY(!archive.open(path, &error));
        QVERIFY(error.message.contains("2,000"));
        entries.clear();
        for (int i = 0; i < 21; ++i)
            entries.append({QByteArray::number(i), "x"});
        auto expanded = zip(entries);
        auto offset = central(expanded);
        for (int i = 0; i < 21; ++i) {
            put32(expanded, offset + 24, quint32(MaxFileBytes));
            offset = expanded.indexOf(QByteArray("PK\1\2", 4), offset + 4);
        }
        QVERIFY(write(path, expanded));
        QVERIFY(!archive.open(path, &error));
        QVERIFY(error.message.contains("100 MiB"));
        QFile large(path);
        QVERIFY(large.open(QIODevice::WriteOnly));
        QVERIFY(large.resize(MaxArchiveBytes + 1));
        large.close();
        QVERIFY(!archive.open(path, &error));
        QVERIFY(error.message.contains("25 MiB"));
    }
    void includes() {
        QTemporaryDir dir;
        QTemporaryDir outside;
        QVERIFY(QDir(dir.path()).mkdir("nested"));
        QVERIFY(write(dir.filePath("base.json"), "{\"color\":1}"));
        QVERIFY(write(dir.filePath("nested/theme.json"), "{\"include\":\"../base.json\"}"));
        IncludeResolver resolver(dir.path());
        Error error;
        QVector<JsonSource> chain;
        QString path;
        QVERIFY(resolver.loadJsonChain("nested/theme.json", &chain, &error));
        QCOMPARE(chain.size(), 2);
        QCOMPARE(chain.first().path, QStringLiteral("base.json"));
        for (auto ref : {"../../escape", "/etc/passwd", "https://example.test/a", "C:\\x"})
            QVERIFY(!resolver.resolve("nested/theme.json", ref, &path, &error));
        QVERIFY(write(dir.filePath("base.json"), "{\"include\":\"nested/./theme.json\"}"));
        QVERIFY(!resolver.loadJsonChain("base.json", &chain, &error));
        QVERIFY(error.message.contains("cycle"));
        QVERIFY(chain.isEmpty());
        for (int i = 0; i < 17; ++i) {
            auto content =
                i == 16 ? QByteArray("{}")
                        : QByteArray("{\"include\":\"") + QByteArray::number(i + 1) + ".json\"}";
            QVERIFY(write(dir.filePath(QString::number(i) + ".json"), content));
        }
        QVERIFY(!resolver.loadJsonChain("0.json", &chain, &error));
        QVERIFY(error.message.contains("16 levels"));
        QVERIFY(resolver.loadJsonChain("1.json", &chain, &error));
        QCOMPARE(chain.size(), 16);
        QVERIFY(write(outside.filePath("outside.json"), "{}"));
        QVERIFY(QFile::link(outside.filePath("outside.json"), dir.filePath("link.json")));
        QVERIFY(!resolver.loadJsonChain("link.json", &chain, &error));
#ifdef Q_OS_UNIX
        QVERIFY(::mkfifo(QFile::encodeName(dir.filePath("pipe.json")).constData(), 0600) == 0);
        QByteArray bytes;
        QVERIFY(!resolver.read("pipe.json", &bytes, &error));
#endif
        QVERIFY(QFile::link(dir.filePath("1.json"), dir.filePath("inside-link.json")));
        QVERIFY(!resolver.loadJsonChain("inside-link.json", &chain, &error));
        QVERIFY(write(dir.filePath("bad.json"), "{\"include\":false}"));
        QVERIFY(!resolver.loadJsonChain("bad.json", &chain, &error));
        QVERIFY(write(dir.filePath("bad.json"), "{\"x\":@}"));
        QVERIFY(!resolver.loadJsonChain("bad.json", &chain, &error));
        QCOMPARE(error.source, QStringLiteral("bad.json"));
    }
};
QTEST_GUILESS_MAIN(ThemeInputTest)
#include "ThemeInputTest.moc"
