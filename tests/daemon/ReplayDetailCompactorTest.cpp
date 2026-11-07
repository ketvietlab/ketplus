#include "storage/ReplayDetailCompactor.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>
using namespace ketplus::daemon;

namespace {
QDateTime cutoff() { return QDateTime::currentDateTimeUtc().addDays(-30); }
QJsonObject tool(const QString& type = "mcpToolCall") {
    return {
        {"id", "item"},
        {"type", type},
        {"status", "failed"},
        {"tool", "lookup"},
        {"server", "synthetic"},
        {"arguments", QJsonObject{{"query", "keep this"}}},
        {"error", QJsonObject{{"code", "keep-error"}}},
        {"result",
         QJsonObject{{"isError", true},
                     {"content", QJsonArray{QJsonObject{
                                     {"type", "text"}, {"text", QString(6000, QChar(0x754c))}}}}}}};
}
} // namespace
class ReplayDetailCompactorTest final : public QObject {
    Q_OBJECT
  private slots:
    void retainsMediaErrorsArgumentsAndStructuredResult() {
        auto original = tool();
        const QJsonObject media{{"type", "image"},
                                {"media_ref", "sha256:synthetic"},
                                {"mimeType", "image/png"},
                                {"data", "keep-inline-image"}};
        const QJsonObject resource{{"type", "resource_link"}, {"uri", "https://example.invalid/a"}};
        auto result = original.value("result").toObject();
        auto content = result.value("content").toArray();
        auto annotated = content.first().toObject();
        annotated.insert("annotations", QJsonObject{{"audience", QJsonArray{"assistant"}}});
        content.replace(0, annotated);
        content.prepend(media);
        content.append(resource);
        content.append(QJsonObject{{"type", "text"}, {"text", "later text"}});
        result.insert("content", content);
        result.insert("structuredContent", QJsonObject{{"opaque", "keep structured data"}});
        original.insert("result", result);
        const auto changed = compactCompletedReplayItem(original, cutoff());
        QVERIFY(changed);
        auto expected = original;
        auto actual = *changed;
        actual.remove("compacted");
        const auto compactedResult = actual.take("result").toObject();
        expected.remove("result");
        QCOMPARE(actual, expected);
        auto kept = compactedResult;
        const auto blocks = kept.take("content").toArray();
        result.remove("content");
        QCOMPARE(kept, result);
        QCOMPARE(blocks.size(), 4);
        QCOMPARE(blocks.first().toObject(), media);
        QCOMPARE(blocks.at(2).toObject(), resource);
        QCOMPARE(blocks.at(1).toObject().value("annotations"), annotated.value("annotations"));
        QVERIFY(blocks.last().toObject().value("text").toString().isEmpty());
        const auto preview = blocks.at(1).toObject().value("text").toString();
        QVERIFY(preview.startsWith("[Tool result trimmed"));
        QVERIFY(!preview.contains(QChar::ReplacementCharacter));
        QVERIFY(!preview.contains("later text"));
        const auto marker = changed->value("compacted").toObject();
        QVERIFY(marker.value("retained_bytes").toInteger() <= 4096);
        QCOMPARE(marker.value("original_bytes").toInteger(), 18010);
        QVERIFY(!compactCompletedReplayItem(*changed, cutoff()));
    }

    void dynamicContentAndStringResultsShareOneBudget() {
        auto original = tool("dynamicToolCall");
        original.insert("result", QString(6000, 'x'));
        const QJsonObject audio{{"type", "inputAudio"}, {"audioUrl", "keep-audio"}};
        original.insert(
            "contentItems",
            QJsonArray{QJsonObject{{"type", "inputText"}, {"text", QString(6000, 'y')}}, audio});
        const auto changed = compactCompletedReplayItem(original, cutoff());
        QVERIFY(changed);
        QCOMPARE(changed->value("compacted").toObject().value("retained_bytes").toInteger(), 4096);
        QVERIFY(changed->value("result").toString().startsWith("[Tool result trimmed"));
        const auto blocks = changed->value("contentItems").toArray();
        QCOMPARE(blocks.last().toObject(), audio);
        QVERIFY(!blocks.first().toObject().value("text").toString().contains(QString(8, 'y')));
    }

    void preservesUnsupportedAndSmallItems_data() {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"small", "nonterminal", "marked", "unknown", "structured-only"})
            QTest::newRow(kind) << QString(kind);
    }
    void preservesUnsupportedAndSmallItems() {
        QFETCH(QString, kind);
        auto item = tool();
        if (kind == "small")
            item.insert("result", "small");
        if (kind == "nonterminal")
            item.insert("status", "inProgress");
        if (kind == "marked")
            item.insert("compacted", QJsonObject{});
        if (kind == "unknown")
            item.insert("type", "unknown");
        if (kind == "structured-only")
            item.insert("result", QJsonObject{{"structuredContent", QString(6000, 'x')}});
        QVERIFY(!compactCompletedReplayItem(item, cutoff()));
    }
    void reasoningKeepsSummaryAndDropsOnlyRawContent() {
        const QJsonArray summary{"Keep this summary"};
        const QJsonObject original{{"id", "reason"},
                                   {"type", "reasoning"},
                                   {"summary", summary},
                                   {"content", QJsonArray{QString(6000, 'x')}},
                                   {"other", "keep"}};
        auto changed = compactCompletedReplayItem(original, cutoff());
        QVERIFY(changed);
        QCOMPARE(changed->value("summary").toArray(), summary);
        QVERIFY(changed->value("content").isArray());
        QVERIFY(changed->value("content").toArray().isEmpty());
        QCOMPARE(changed->value("other"), original.value("other"));
        QCOMPARE(changed->value("compacted").toObject().value("retained_bytes").toInteger(), 0);
        QVERIFY(!compactCompletedReplayItem(*changed, cutoff()));
        auto missing = original;
        missing.insert("summary", QJsonArray{});
        QVERIFY(!compactCompletedReplayItem(missing, cutoff()));
        missing.insert("summary", QJsonArray{"   "});
        QVERIFY(!compactCompletedReplayItem(missing, cutoff()));
        missing = original;
        missing.insert("status", "inProgress");
        QVERIFY(!compactCompletedReplayItem(missing, cutoff()));
        missing = original;
        missing.insert("content", QJsonArray{QJsonObject{{"media_ref", "keep-unknown-shape"}}});
        QVERIFY(!compactCompletedReplayItem(missing, cutoff()));
    }
    void invalidCutoffDoesNotTransform() {
        QVERIFY(!compactCompletedReplayItem(tool(), {}));
        QVERIFY(!compactCompletedReplayItem(tool(), QDateTime::currentDateTimeUtc().addDays(1)));
    }
};
QTEST_GUILESS_MAIN(ReplayDetailCompactorTest)
#include "ReplayDetailCompactorTest.moc"
