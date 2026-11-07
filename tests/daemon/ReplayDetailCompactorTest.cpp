#include "storage/ReplayDetailCompactor.h"
#include "agent/FileChangeStatistics.h"
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
    void fileDiffPreservesIdentityAndFullStatistics_data() {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"add", "delete", "update", "move", "unknown"})
            QTest::newRow(kind) << QString(kind);
    }
    void fileDiffPreservesIdentityAndFullStatistics() {
        QFETCH(QString, kind);
        QString diff;
        if (kind == "update" || kind == "move")
            diff = "diff --git a/old b/new\n--- a/old\n+++ b/new\n@@ -1,5000 +1,5000 @@\n";
        for (int i = 0; i < 5000; ++i)
            diff += QStringLiteral("+界 added line\n-removed line\n");
        QJsonObject change{
            {"path", "src/old.cpp"},
            {"kind", QJsonObject{{"type", kind == "move" ? "update" : kind},
                                 {"move_path", kind == "move" ? "src/new.cpp" : ""}}},
            {"diff", diff},
            {"opaque", "keep"}};
        const QJsonObject small{{"path", "small"}, {"diff", "keep small"}};
        const QJsonObject original{{"id", "edit"},
                                   {"type", "fileChange"},
                                   {"status", "completed"},
                                   {"changes", QJsonArray{change, small}},
                                   {"other", "keep"}};
        const auto changed = compactCompletedReplayItem(original, cutoff());
        QVERIFY(changed);
        const auto changes = changed->value("changes").toArray();
        QCOMPARE(changes.at(1).toObject(), small);
        auto retained = changes.first().toObject();
        QVERIFY(!retained.contains("diff"));
        const auto preview = retained.take("diff_excerpt").toString();
        QVERIFY(preview.toUtf8().size() <= 63 * 1024);
        QVERIFY(diff.startsWith(preview));
        QVERIFY(!preview.contains(QChar::ReplacementCharacter));
        const auto stats = ketplus::fileChangeStatistics(changes.first().toObject());
        QCOMPARE(stats.known, kind != "unknown");
        QCOMPARE(stats.added, kind == "add"                        ? 10000
                              : kind == "update" || kind == "move" ? 5000
                                                                   : 0);
        QCOMPARE(stats.removed, kind == "delete"                     ? 10000
                                : kind == "update" || kind == "move" ? 5000
                                                                     : 0);
        QVERIFY(!ketplus::rawFileChangeStatistics(changes.first().toObject()).known);
        const auto info = retained.take("compacted").toObject();
        QCOMPARE(info.value("original_bytes").toInteger(), diff.toUtf8().size());
        QCOMPARE(info.value("retained_bytes").toInteger(), preview.toUtf8().size());
        QVERIFY(info.value("excerpt_only").toBool());
        change.remove("diff");
        QCOMPARE(retained, change);
        auto remaining = *changed;
        remaining.remove("changes");
        remaining.remove("compacted");
        auto expected = original;
        expected.remove("changes");
        QCOMPARE(remaining, expected);
        QVERIFY(!compactCompletedReplayItem(*changed, cutoff()));
    }
    void fileDiffCutsAtUtf8Boundary() {
        const QString prefix(63 * 1024 - 1, 'x');
        const QString diff = prefix + QString(1000, QChar(0x754c));
        const QJsonObject item{
            {"type", "fileChange"},
            {"status", "failed"},
            {"changes", QJsonArray{QJsonObject{{"path", "utf8"}, {"diff", diff}}}}};
        const auto changed = compactCompletedReplayItem(item, cutoff());
        QVERIFY(changed);
        QCOMPARE(
            changed->value("changes").toArray().first().toObject().value("diff_excerpt").toString(),
            prefix);
    }
    void fileDiffRejectsAmbiguousShapes() {
        QJsonObject change{{"diff", QString(65536, 'x')}};
        QJsonObject item{
            {"type", "fileChange"}, {"status", "completed"}, {"changes", QJsonArray{change}}};
        QVERIFY(!compactCompletedReplayItem(item, cutoff()));
        change.insert("diff", QString(70000, 'x'));
        for (const char* field : {"compacted", "diff_excerpt"}) {
            auto marked = change;
            marked.insert(field, QJsonObject{});
            item.insert("changes", QJsonArray{marked});
            QVERIFY(!compactCompletedReplayItem(item, cutoff()));
        }
        item.insert("changes", QJsonArray{change});
        item.insert("status", "inProgress");
        QVERIFY(!compactCompletedReplayItem(item, cutoff()));
    }
    void storedStatisticsRejectInvalidCounts() {
        for (const auto& invalid : QJsonArray{-1, 1.5, 2147483648.0, "3", QJsonValue()}) {
            const QJsonObject change{
                {"diff_excerpt", "@@ -1 +1 @@\n+partial\n"},
                {"compacted",
                 QJsonObject{
                     {"policy", "file-diff-v1"},
                     {"stats", QJsonObject{{"known", true}, {"added", invalid}, {"removed", 0}}}}}};
            QVERIFY(!ketplus::fileChangeStatistics(change).known);
        }
    }
    void invalidCutoffDoesNotTransform() {
        QVERIFY(!compactCompletedReplayItem(tool(), {}));
        QVERIFY(!compactCompletedReplayItem(tool(), QDateTime::currentDateTimeUtc().addDays(1)));
    }
};
QTEST_GUILESS_MAIN(ReplayDetailCompactorTest)
#include "ReplayDetailCompactorTest.moc"
