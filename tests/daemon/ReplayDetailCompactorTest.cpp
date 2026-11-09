#include "storage/ReplayDetailCompactor.h"
#include "agent/FileChangeStatistics.h"
#include "agent/TranscriptSanitizer.h"
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
    void structuredResultsAreExplicitSanitizedAndIdempotent_data() {
        QTest::addColumn<QString>("policy");
        for (const char* policy : {"", "tool-result-text-v1", "tool-arguments-v1"})
            QTest::newRow(*policy ? policy : "fresh") << QString(policy);
    }
    void structuredResultsAreExplicitSanitizedAndIdempotent() {
        QFETCH(QString, policy);
        auto original = tool();
        original.insert("status", "completed");
        original.remove("error");
        const QJsonObject structured{{"access_token", "synthetic-secret"},
                                     {"rows", QJsonArray{QString(6000, QChar(0x754c))}}};
        original.insert("result",
                        QJsonObject{{"structuredContent", structured},
                                    {"content", QJsonArray{QJsonObject{{"type", "image"},
                                                                       {"data", "keep-image"}}}},
                                    {"isError", false},
                                    {"extension", "keep"}});
        if (!policy.isEmpty())
            original.insert("compacted", QJsonObject{{"policy", policy}, {"original_bytes", 123}});
        const auto changed = compactCompletedReplayItem(original, cutoff());
        QVERIFY(changed);
        auto result = changed->value("result").toObject();
        QVERIFY(!result.contains("structuredContent"));
        const auto excerpt = result.take("structuredContent_excerpt").toString();
        QVERIFY(excerpt.startsWith("[Structured result trimmed"));
        QVERIFY(!excerpt.contains("synthetic-secret"));
        QVERIFY(!excerpt.contains(QChar::ReplacementCharacter));
        const auto retained = excerpt.mid(excerpt.indexOf('\n') + 1).toUtf8();
        QVERIFY(retained.size() <= 4096);
        auto expected = original.value("result").toObject();
        expected.remove("structuredContent");
        // The inline image block keeps its place without its pixels (ADR 0099).
        expected.insert("content", QJsonArray{QJsonObject{{"type", "image"},
                                                          {"omitted", true},
                                                          {"reason", "history_retention"},
                                                          {"original_bytes", 10}}});
        QCOMPARE(result, expected);
        auto unchanged = *changed;
        unchanged.remove("result");
        const auto info = unchanged.take("compacted").toObject();
        auto originalRest = original;
        originalRest.remove("result");
        originalRest.remove("compacted");
        QCOMPARE(unchanged, originalRest);
        const auto details = info.value("structured_result").toObject();
        QCOMPARE(details.value("retained_bytes").toInteger(), retained.size());
        QCOMPARE(details.value("original_bytes").toInteger(),
                 QJsonDocument(structured).toJson(QJsonDocument::Compact).size());
        QVERIFY(details.value("redacted_before_excerpt").toBool());
        QVERIFY(details.value("excerpt_only").toBool());
        if (!policy.isEmpty()) {
            QCOMPARE(info.value("policy"), QJsonValue(policy));
            QCOMPARE(info.value("original_bytes"), QJsonValue(123));
        }
        QVERIFY(!compactCompletedReplayItem(*changed, cutoff()));
    }
    void structuredArraysShareTheTextBudget() {
        auto original = tool("dynamicToolCall");
        original.insert("status", "completed");
        original.remove("error");
        original.insert(
            "result",
            QJsonObject{{"structuredContent", QJsonArray{QString(6000, 'x')}},
                        {"content",
                         QJsonArray{QJsonObject{{"type", "text"}, {"text", QString(3000, 't')}}}}});
        const auto changed = compactCompletedReplayItem(original, cutoff());
        QVERIFY(changed);
        const auto result = changed->value("result").toObject();
        QCOMPARE(result.value("content"), original.value("result").toObject().value("content"));
        QCOMPARE(changed->value("compacted")
                     .toObject()
                     .value("structured_result")
                     .toObject()
                     .value("retained_bytes")
                     .toInteger(),
                 1096);
        QVERIFY(!compactCompletedReplayItem(*changed, cutoff()));
    }
    void structuredResultsKeepErrorsResourcesAndUnknownMarkers_data() {
        QTest::addColumn<QString>("guard");
        for (const char* guard :
             {"small", "failed", "running", "error", "unknown", "excerpt", "media", "resource",
              "uri", "inline", "deep", "malformed-error", "item-error"})
            QTest::newRow(guard) << QString(guard);
    }
    void structuredResultsKeepErrorsResourcesAndUnknownMarkers() {
        QFETCH(QString, guard);
        auto original = tool();
        original.insert("status", "completed");
        original.remove("error");
        QJsonObject structured{{"rows", QString(6000, 'x')}};
        if (guard == "small")
            structured.insert("rows", "small");
        if (guard == "media")
            structured.insert("nested", QJsonObject{{"type", "image"}});
        if (guard == "resource")
            structured.insert("resource", "keep");
        if (guard == "uri")
            structured.insert("uri", "asset:keep");
        if (guard == "inline")
            structured.insert("value", "data:image/png;base64,keep");
        if (guard == "deep")
            for (int n = 0; n < 66; ++n)
                structured = {{"nested", structured}};
        QJsonObject result{{"structuredContent", structured}};
        if (guard == "excerpt")
            result.insert("structuredContent_excerpt", "existing");
        if (guard == "error")
            result.insert("isError", true);
        if (guard == "item-error")
            original.insert("error", QJsonObject{{"message", "keep details"}});
        if (guard == "malformed-error")
            result.insert("isError", "false");
        if (guard == "failed")
            original.insert("status", "failed");
        if (guard == "running")
            original.insert("status", "inProgress");
        if (guard == "unknown")
            original.insert("compacted", QJsonObject{{"policy", "future"}});
        original.insert("result", result);
        QVERIFY(!compactCompletedReplayItem(original, cutoff()));
    }
    void argumentExcerptsAreBoundedExplicitAndIdempotent_data() {
        QTest::addColumn<QString>("shape");
        QTest::addColumn<bool>("previous");
        for (const QString shape : {QString("object"), QString("array"), QString("string")})
            for (bool previous : {false, true})
                QTest::newRow(qPrintable(shape + (previous ? "-previous-text" : "-fresh")))
                    << shape << previous;
    }
    void argumentExcerptsAreBoundedExplicitAndIdempotent() {
        QFETCH(QString, shape);
        QFETCH(bool, previous);
        auto original = tool(shape == "string" ? "dynamicToolCall" : "mcpToolCall");
        const QString longText(6000, QChar(0x754c));
        original.insert("arguments", shape == "object"
                                         ? QJsonValue(QJsonObject{{"query", longText}})
                                     : shape == "array" ? QJsonValue(QJsonArray{longText})
                                                        : QJsonValue(longText));
        if (previous) {
            original.insert("result", "[Tool result trimmed] retained text");
            original.insert("compacted", QJsonObject{{"policy", "tool-result-text-v1"},
                                                     {"original_bytes", 50000},
                                                     {"retained_bytes", 100}});
        }
        const auto changed = compactCompletedReplayItem(original, cutoff());
        QVERIFY(changed);
        QVERIFY(!changed->contains("arguments"));
        const auto excerpt = changed->value("arguments_excerpt").toString();
        QVERIFY(excerpt.startsWith("[Arguments trimmed"));
        QVERIFY(!excerpt.contains(QChar::ReplacementCharacter));
        const auto retained = excerpt.mid(excerpt.indexOf('\n') + 1).toUtf8();
        QVERIFY(retained.size() <= 4096);
        const auto originalValue = original.value("arguments");
        const auto originalBytes =
            originalValue.isString() ? originalValue.toString().toUtf8()
            : originalValue.isArray()
                ? QJsonDocument(originalValue.toArray()).toJson(QJsonDocument::Compact)
                : QJsonDocument(originalValue.toObject()).toJson(QJsonDocument::Compact);
        QVERIFY(originalBytes.startsWith(retained));
        const auto info = changed->value("compacted").toObject();
        QCOMPARE(info.value("policy"), QJsonValue("tool-result-text-v1"));
        const auto args = info.value("arguments").toObject();
        QCOMPARE(args.value("retained_bytes").toInteger(), retained.size());
        QVERIFY(args.value("excerpt_only").toBool());
        QCOMPARE(changed->value("error"), original.value("error"));
        QCOMPARE(changed->value("tool"), original.value("tool"));
        if (previous) {
            QCOMPARE(changed->value("result"), original.value("result"));
            QCOMPARE(info.value("original_bytes").toInteger(), 50000);
        }
        QVERIFY(!compactCompletedReplayItem(*changed, cutoff()));
    }
    void argumentOnlyCapPreservesResultAndUnknownMarkers() {
        auto original = tool();
        original.insert("arguments", QJsonObject{{"query", QString(6000, 'x')}});
        original.insert("result", QJsonObject{{"structuredContent", "keep"}, {"isError", true}});
        auto changed = compactCompletedReplayItem(original, cutoff());
        QVERIFY(changed);
        QCOMPARE(changed->value("result"), original.value("result"));
        QCOMPARE(changed->value("compacted").toObject().value("policy"),
                 QJsonValue("tool-arguments-v1"));
        for (const QJsonValue value : {QJsonValue(QJsonObject{}), QJsonValue("unknown"),
                                       QJsonValue(QJsonObject{{"policy", "future"}})}) {
            original.insert("compacted", value);
            QVERIFY(!compactCompletedReplayItem(original, cutoff()));
        }
        original.remove("compacted");
        original.insert("status", "inProgress");
        QVERIFY(!compactCompletedReplayItem(original, cutoff()));
        original.insert("status", "completed");
        original.insert("arguments_excerpt", "existing unknown excerpt");
        QVERIFY(!compactCompletedReplayItem(original, cutoff()));
    }
    void argumentCapRejectsOtherKindsAndInvalidTime() {
        auto original = tool();
        original.insert("arguments", QString(7000, 'x'));
        original.insert("result", "small");
        QVERIFY(!compactCompletedReplayItem(original, {}));
        QVERIFY(!compactCompletedReplayItem(original, QDateTime::currentDateTimeUtc().addDays(1)));
        for (const QString kind :
             {QString("userMessage"), QString("agentMessage"), QString("reasoning"),
              QString("question"), QString("commandExecution")}) {
            original.insert("type", kind);
            QVERIFY(!compactCompletedReplayItem(original, cutoff()));
        }
    }
    void argumentExcerptsRedactBeforeFlatteningAndTruncating_data() {
        QTest::addColumn<bool>("encoded");
        QTest::newRow("object") << false;
        QTest::newRow("encoded-json") << true;
    }
    void argumentExcerptsRedactBeforeFlatteningAndTruncating() {
        QFETCH(bool, encoded);
        const QJsonObject args{
            {"password", "synthetic-password-value"},
            {"nested",
             QJsonObject{
                 {"access_token", "synthetic-token-value"},
                 {"url", "https://user:pass@example.invalid/?api_key=synthetic-query-value"}}},
            {"media", QJsonObject{{"type", "image"}, {"data", "synthetic-image-bytes"}}},
            {"z_padding", QString(7000, 'x')}};
        auto original = tool();
        original.insert("result", "keep result");
        original.insert("arguments", encoded
                                         ? QJsonValue(QString::fromUtf8(
                                               QJsonDocument(args).toJson(QJsonDocument::Compact)))
                                         : QJsonValue(args));
        const auto changed = compactCompletedReplayItem(original, cutoff());
        QVERIFY(changed);
        const auto excerpt = changed->value("arguments_excerpt").toString();
        for (const QString secret :
             {QString("synthetic-password-value"), QString("synthetic-token-value"),
              QString("synthetic-query-value"), QString("user:pass"),
              QString("synthetic-image-bytes")})
            QVERIFY2(!excerpt.contains(secret), qPrintable(secret));
        QVERIFY(excerpt.contains("[redacted]"));
        QVERIFY(changed->value("compacted")
                    .toObject()
                    .value("arguments")
                    .toObject()
                    .value("redacted_before_excerpt")
                    .toBool());
    }
    void sharedSanitizerKeepsTextAndMultipleUrlBoundaries() {
        const QString input = "before https://user:pass@example.invalid/?token=secret between "
                              "https://example.invalid/path?q=ok after";
        QCOMPARE(ketplus::TranscriptSanitizer::text(input),
                 QString("before https://redacted:redacted@example.invalid/?token=[redacted] "
                         "between https://example.invalid/path?q=ok after"));
        const QString plain = QString(7000, 'x') + " no URLs or secrets";
        QCOMPARE(ketplus::TranscriptSanitizer::text(plain), plain);
        const QString markdown = "[link](https://example.invalid/a?token=secret) and "
                                 "\"https://example.invalid/b?key=secret\".";
        const auto safe = ketplus::TranscriptSanitizer::text(markdown);
        QVERIFY(!safe.contains("secret"));
        QVERIFY(safe.startsWith("[link]("));
        QVERIFY(safe.contains(" and "));
    }
    void argumentSanitizerHeavyInput() {
        auto original = tool();
        original.insert("result", "keep result");
        original.insert(
            "arguments",
            QString("https://user:synthetic-pass@example.invalid/?token=synthetic-token ")
                .repeated(8000));
        QElapsedTimer timer;
        timer.start();
        const auto changed = compactCompletedReplayItem(original, cutoff());
        qInfo() << "Argument sanitizer, 8000 credential-bearing URLs:" << timer.elapsed() << "ms";
        QVERIFY(changed);
        const auto excerpt = changed->value("arguments_excerpt").toString();
        QVERIFY(!excerpt.contains("synthetic-pass"));
        QVERIFY(!excerpt.contains("synthetic-token"));
        QVERIFY(!changed->contains("arguments"));
    }
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
    void toolResultImagesBecomeOmittedBlocks() {
        const QJsonObject png{{"type", "image"}, {"mimeType", "image/png"}, {"data", "QUJD"}};
        const QJsonObject referenced{{"type", "image"}, {"media_ref", "m"}};
        const QJsonObject text{{"type", "text"}, {"text", "short"}};
        const QJsonObject legacy{
            {"id", "tool-1"},
            {"type", "mcpToolCall"},
            {"status", "completed"},
            {"server", "browser"},
            {"tool", "screenshot"},
            {"result", QJsonObject{{"content", QJsonArray{text, png, referenced}}}}};
        const auto item = compactCompletedReplayItem(legacy, cutoff());
        QVERIFY(item);
        const auto content = item->value("result").toObject().value("content").toArray();
        QCOMPARE(content.size(), 3);
        QCOMPARE(content.at(0).toObject(), text);
        const auto omitted = content.at(1).toObject();
        QCOMPARE(omitted.value("type").toString(), QString("image"));
        QVERIFY(omitted.value("omitted").toBool());
        QCOMPARE(omitted.value("reason").toString(), QString("history_retention"));
        QCOMPARE(omitted.value("mimeType").toString(), QString("image/png"));
        QVERIFY(!omitted.contains("data"));
        QCOMPARE(content.at(2).toObject(), referenced);
        const auto marker = item->value("compacted").toObject();
        QCOMPARE(marker.value("policy").toString(), QString("tool-inline-images-v1"));
        QCOMPARE(marker.value("inline_images").toObject().value("original_bytes").toInteger(), 4);
        QVERIFY(!compactCompletedReplayItem(*item, cutoff()));

        // A result already trimmed by the text policy keeps that marker and gains this one.
        auto trimmed = legacy;
        trimmed.insert("compacted", QJsonObject{{"policy", "tool-result-text-v1"}});
        const auto upgraded = compactCompletedReplayItem(trimmed, cutoff());
        QVERIFY(upgraded);
        QCOMPARE(upgraded->value("compacted").toObject().value("policy").toString(),
                 QString("tool-result-text-v1"));
        QVERIFY(upgraded->value("compacted").toObject().contains("inline_images"));
        // Unknown policies, running calls and invalid cutoffs stay as they are.
        auto unknown = legacy;
        unknown.insert("compacted", QJsonObject{{"policy", "future-v9"}});
        QVERIFY(!compactCompletedReplayItem(unknown, cutoff()));
        auto running = legacy;
        running.insert("status", "inProgress");
        QVERIFY(!compactCompletedReplayItem(running, cutoff()));
        QVERIFY(!compactCompletedReplayItem(legacy, QDateTime()));

        // Dynamic tools: only data URLs go; a remote URL is a reference, not pixels.
        const QJsonObject dynamic{
            {"id", "tool-2"},
            {"type", "dynamicToolCall"},
            {"status", "completed"},
            {"contentItems",
             QJsonArray{
                 QJsonObject{{"type", "inputImage"}, {"imageUrl", "data:image/png;base64,QUJD"}},
                 QJsonObject{{"type", "inputImage"}, {"imageUrl", "https://example.test/a.png"}}}}};
        const auto stripped = compactCompletedReplayItem(dynamic, cutoff());
        QVERIFY(stripped);
        const auto items = stripped->value("contentItems").toArray();
        QVERIFY(items.at(0).toObject().value("omitted").toBool());
        QCOMPARE(items.at(1).toObject(), dynamic.value("contentItems").toArray().at(1).toObject());
    }

    void generatedImageLosesOnlyItsInlinePixels() {
        const QJsonObject legacy{{"id", "image-1"},
                                 {"type", "imageGeneration"},
                                 {"status", "completed"},
                                 {"revisedPrompt", "a red kite over the sea"},
                                 {"savedPath", "/synthetic/kite.png"},
                                 {"result", QString(4096, 'A')}};
        const auto item = compactCompletedReplayItem(legacy, cutoff());
        QVERIFY(item);
        QVERIFY(!item->contains("result"));
        for (const char* kept : {"id", "type", "status", "revisedPrompt", "savedPath"})
            QCOMPARE(item->value(kept), legacy.value(kept));
        const auto marker = item->value("compacted").toObject();
        QCOMPARE(marker.value("policy").toString(), QString("image-pixels-v1"));
        QCOMPARE(marker.value("original_bytes").toInteger(), 4096);
        QCOMPARE(marker.value("retained_bytes").toInteger(), 0);
        // Idempotent, and nothing else is touched.
        QVERIFY(!compactCompletedReplayItem(*item, cutoff()));
        auto referenced = legacy;
        referenced.insert("result",
                          QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "image"},
                                                                         {"media_ref", "m"}}}}});
        QVERIFY(!compactCompletedReplayItem(referenced, cutoff()));
        auto noPixels = legacy;
        noPixels.remove("result");
        QVERIFY(!compactCompletedReplayItem(noPixels, cutoff()));
        QVERIFY(!compactCompletedReplayItem(legacy, QDateTime()));
        QVERIFY(!compactCompletedReplayItem(legacy, QDateTime::currentDateTimeUtc().addDays(1)));
    }
};
QTEST_GUILESS_MAIN(ReplayDetailCompactorTest)
#include "ReplayDetailCompactorTest.moc"
