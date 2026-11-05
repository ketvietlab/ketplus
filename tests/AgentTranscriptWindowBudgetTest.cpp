#include "agent/AgentTranscriptWindowBudget.h"
#include "agent/AgentTranscriptPaging.h"

#include <QtTest>

namespace tw = ketplus::transcript_window;

namespace {
QVariantMap row(const QString& key, const QString& turn = {}, qsizetype bodyBytes = 8) {
    QVariantMap message{{"messageKey", key}, {"body", QString(bodyBytes, QLatin1Char('a'))}};
    if (!turn.isEmpty())
        message.insert("turnId", turn);
    return message;
}
qsizetype totalBytes(const QVector<QVariantMap>& messages) {
    qsizetype bytes = 0;
    for (const auto& message : messages)
        bytes += tw::sourceBytes(message);
    return bytes;
}
} // namespace

class AgentTranscriptWindowBudgetTest final : public QObject {
    Q_OBJECT
  private slots:
    void pageTurnLimitFillsThreeViewportsWithinBounds() {
        namespace tp = ketplus::transcript_paging;
        // Unknown geometry keeps the historical three-turn page.
        QCOMPARE(tp::adaptiveTurnLimit(0, 0, 0, 20), 3);
        QCOMPARE(tp::adaptiveTurnLimit(700, -1, 2, 20), 3);
        // Tall turns (1000 px) need the minimum; short turns fill 3 x 700 px.
        QCOMPARE(tp::adaptiveTurnLimit(700, 500, 2, 20), 3);
        QCOMPARE(tp::adaptiveTurnLimit(700, 100, 2, 20), 11); // ceil(2100 / 200)
        QCOMPARE(tp::adaptiveTurnLimit(700, 50, 2, 20), 20);  // ceil(21) clamped
        QCOMPARE(tp::adaptiveTurnLimit(700, 1e-9, 2, 20), 20);
        // A legacy daemon's ceiling wins over the estimate.
        QCOMPARE(tp::adaptiveTurnLimit(700, 50, 2, 3), 3);
        QCOMPARE(tp::adaptiveTurnLimit(700, 50, 2, 200), 20);
        QVERIFY(tp::adaptiveTurnLimit(1400, 100, 2, 20) > tp::adaptiveTurnLimit(700, 100, 2, 20));
    }

    void sourceBytesCountsUtf8WithoutEncoding() {
        const QString text =
            QStringLiteral("a\u00e9\u20ac") + QString::fromUcs4(U"\U0001F600"); // 1 + 2 + 3 + 4
        QVariantMap message{{"body", text}, {"turnId", QStringLiteral("t")}, {"count", 7}};
        QCOMPARE(tw::sourceBytes(message), qsizetype(text.toUtf8().size() + 1));
        QCOMPARE(tw::sourceBytes(message), qsizetype(11));
        // A lone surrogate is counted as the three bytes of a replacement character.
        QCOMPARE(tw::sourceBytes({{"body", QString(QChar(0xD800))}}), qsizetype(3));
        // Nested values count as their compact JSON size.
        const qsizetype before = tw::sourceBytes(message);
        message.insert("meta", QVariantMap{{"k", QStringLiteral("value")}});
        QCOMPARE(tw::sourceBytes(message) - before,
                 qsizetype(QByteArray(R"({"k":"value"})").size()));
        message.insert("items",
                       QVariantList{QStringLiteral("x"), QVariantMap{{"a", QStringLiteral("b")}}});
        QCOMPARE(tw::sourceBytes(message) - before - 13,
                 qsizetype(QByteArray(R"(["x",{"a":"b"}])").size()));
    }

    void batchReadsUntilItFillsTheBuffer() {
        namespace tp = ketplus::transcript_paging;
        // The configured page count is always read.
        QVERIFY(tp::batchWantsMore(2, 5, 1000, 1000, 100));
        // Unknown geometry stops at that count.
        QVERIFY(!tp::batchWantsMore(5, 5, 10, 0, 100));
        QVERIFY(!tp::batchWantsMore(5, 5, 10, 1000, 0));
        // 50 rows x 100 px = 5,000 px, short of 6 x 1000: read on.
        QVERIFY(tp::batchWantsMore(5, 5, 50, 1000, 100));
        QVERIFY(!tp::batchWantsMore(5, 5, 60, 1000, 100));
        // Bounded however little a page holds.
        QVERIFY(!tp::batchWantsMore(tp::maximumBatchPages, 5, 1, 1000, 100));
        // A page count of 1 (tests walking one page at a time) is honoured only
        // when geometry is unknown.
        QVERIFY(!tp::batchWantsMore(1, 1, 3, 0, 0));
    }

    void windowBudgetFollowsRenderedHeight() {
        namespace tw = ketplus::transcript_window;
        // Unknown geometry keeps the fixed count.
        QCOMPARE(tw::messageBudget(0, 300, 948), tw::maximumMessages);
        QCOMPARE(tw::messageBudget(2400, 0, 948), tw::maximumMessages);
        // Compact rows: 300 messages in 12,000 px need 356 for 15 viewports.
        QCOMPARE(tw::messageBudget(12000, 300, 948), 356);
        // Text-heavy: 300 messages already fill 16,000 px, so the floor holds.
        QCOMPARE(tw::messageBudget(16000, 300, 948), tw::maximumMessages);
        // Rows of almost no height hit the ceiling.
        QCOMPARE(tw::messageBudget(10, 300, 948), tw::largestMessageBudget);
        // A trim honours the limit it is given.
        QVector<QVariantMap> rows;
        for (int index = 0; index < 900; ++index)
            rows.append({{"messageKey", QString::number(index)},
                         {"turnId", QString("turn:%1").arg(index / 3)}});
        QVERIFY(tw::trim(rows, tw::RetainedEdge::Newest, 600).removedOlder);
        QCOMPARE(rows.size(), 600);
    }

    void windowParamsSelectDirectionAndFencing() {
        namespace tp = ketplus::transcript_paging;
        const auto initial = tp::windowParams(tp::Direction::Older, {}, 40, 2);
        QCOMPARE(initial.value("before_cursor").toInteger(), 0);
        QCOMPARE(initial.value("turn_limit").toInt(), 3);
        QCOMPARE(initial.value("event_limit").toInt(), 128);
        QVERIFY(!initial.contains("snapshot_cursor"));
        QVERIFY(!initial.contains("projection_revision"));
        QVERIFY(!initial.contains("after_cursor"));
        const auto older = tp::windowParams(tp::Direction::Older, "17", 40, 2, 11);
        QCOMPARE(older.value("before_cursor").toInteger(), 17);
        QCOMPARE(older.value("snapshot_cursor").toInteger(), 40);
        QCOMPARE(older.value("projection_revision").toInteger(), 2);
        QCOMPARE(older.value("turn_limit").toInt(), 11);
        // An older page resuming at a live event past the snapshot is fenced to cover it.
        const auto live = tp::windowParams(tp::Direction::Older, "58", 40, 2);
        QCOMPARE(live.value("before_cursor").toInteger(), 58);
        QCOMPARE(live.value("snapshot_cursor").toInteger(), 57);
        QCOMPARE(live.value("projection_revision").toInteger(), 2);
        QCOMPARE(tp::windowParams(tp::Direction::Older, "41", 40, 2)
                     .value("snapshot_cursor")
                     .toInteger(),
                 40);
        const auto newer = tp::windowParams(tp::Direction::Newer, "25", 40, 2, 99);
        QCOMPARE(newer.value("after_cursor").toInteger(), 25);
        QVERIFY(!newer.contains("before_cursor"));
        QCOMPARE(newer.value("snapshot_cursor").toInteger(), 40);
        QCOMPARE(newer.value("turn_limit").toInt(), 20);
        // after_cursor is a positive event cursor: never sent as 0.
        QVERIFY(!tp::windowParams(tp::Direction::Newer, {}, 40, 2).contains("after_cursor"));
        QVERIFY(!tp::windowParams(tp::Direction::Newer, "0", 40, 2).contains("after_cursor"));
    }

    void turnlessRowsEvictExactlyTheOverflow() {
        for (const auto edge : {tw::RetainedEdge::Newest, tw::RetainedEdge::Oldest}) {
            QVector<QVariantMap> messages;
            const int total = tw::maximumMessages + 5;
            for (int i = 0; i < total; ++i)
                messages.append(row(QString::number(i)));
            const auto result = tw::trim(messages, edge);
            QVERIFY(result.changed);
            QCOMPARE(messages.size(), tw::maximumMessages);
            if (edge == tw::RetainedEdge::Newest) {
                QVERIFY(result.removedOlder && !result.removedNewer);
                QCOMPARE(messages.constFirst().value("messageKey").toString(), QStringLiteral("5"));
                QCOMPARE(messages.constLast().value("messageKey").toString(),
                         QString::number(total - 1));
            } else {
                QVERIFY(result.removedNewer && !result.removedOlder);
                QCOMPARE(messages.constFirst().value("messageKey").toString(), QStringLiteral("0"));
                QCOMPARE(messages.constLast().value("messageKey").toString(),
                         QString::number(tw::maximumMessages - 1));
            }
        }
    }

    void readingRowSurvivesEitherPagingDirectionAndByteLimit() {
        for (auto edge : {tw::RetainedEdge::Oldest, tw::RetainedEdge::Newest}) {
            for (int reader : {0, 3, 40, 96, 99}) {
                QVector<QVariantMap> messages;
                for (int i = 0; i < 100; ++i)
                    messages.append(row(QString::number(i), "long-turn", 1000));
                const auto result = tw::trim(messages, edge, 20, 12000, QString::number(reader));
                QVERIFY(result.changed);
                QVERIFY(messages.size() <= 20);
                QVERIFY(totalBytes(messages) <= 12000);
                bool found = false;
                for (int i = 0; i < messages.size(); ++i) {
                    const auto key = messages.at(i).value("messageKey").toString();
                    found |= key == QString::number(reader);
                    if (i > 0)
                        QCOMPARE(key.toInt(), messages.at(i - 1).value("messageKey").toInt() + 1);
                }
                QVERIFY(found);
            }
        }
    }

    void completeTurnsAreEvictedTogether() {
        QVector<QVariantMap> messages;
        for (int i = 0; i < 3; ++i)
            messages.append(row(QStringLiteral("t0-%1").arg(i), "t0"));
        for (int i = 0; i < tw::maximumMessages; ++i)
            messages.append(row(QString::number(i)));
        tw::trim(messages, tw::RetainedEdge::Newest);
        QCOMPARE(messages.size(), tw::maximumMessages);
        QCOMPARE(messages.constFirst().value("messageKey").toString(), QStringLiteral("0"));
    }

    void singleOversizedTurnIsHardBoundedKeepingNewest() {
        QVector<QVariantMap> messages;
        const qsizetype chunk = tw::maximumUtf8Bytes / 4;
        for (int i = 0; i < 10; ++i)
            messages.append(row(QString::number(i), "only-turn", chunk));
        auto oldest = messages;
        const auto result = tw::trim(messages, tw::RetainedEdge::Newest);
        QVERIFY(result.changed && result.removedOlder);
        QVERIFY(totalBytes(messages) <= tw::maximumUtf8Bytes);
        QVERIFY(!messages.isEmpty());
        QCOMPARE(messages.constLast().value("messageKey").toString(), QStringLiteral("9"));

        tw::trim(oldest, tw::RetainedEdge::Oldest);
        QVERIFY(totalBytes(oldest) <= tw::maximumUtf8Bytes);
        QCOMPARE(oldest.constFirst().value("messageKey").toString(), QStringLiteral("0"));
    }

    void oversizedRetainedEdgeRowDoesNotShieldTheRestOfItsTurn() {
        // 17 MB prompt followed by 250 x 1 MB tool outputs in the same turn, paged from the
        // oldest edge: only the prompt may exceed the cap, the outputs are still trimmed.
        QVector<QVariantMap> messages{row("prompt", "turn", 17 * 1000 * 1000)};
        for (int i = 0; i < 250; ++i)
            messages.append(row(QStringLiteral("tool-%1").arg(i), "turn", 1000 * 1000));
        const auto result = tw::trim(messages, tw::RetainedEdge::Oldest);
        QVERIFY(result.changed && result.removedNewer);
        QCOMPARE(messages.constFirst().value("messageKey").toString(), QStringLiteral("prompt"));
        const qsizetype promptBytes = tw::sourceBytes(messages.constFirst());
        QVERIFY(totalBytes(messages) <= promptBytes + tw::oversizedCompanionBytes);

        // A small companion row stays beside an oversized retained-edge row.
        QVector<QVariantMap> answer{row("prompt", "t", 16),
                                    row("answer", "t", tw::maximumUtf8Bytes + 10)};
        QVERIFY(!tw::trim(answer, tw::RetainedEdge::Newest).changed);
        QCOMPARE(answer.size(), 2);
    }

    void unboundedByteLimitTrimsByCountOnly() {
        // Two turns of 10 MB each are over the byte bound but within the count.
        QVector<QVariantMap> messages{row("a", "t1", 10 * 1000 * 1000),
                                      row("b", "t2", 10 * 1000 * 1000)};
        QVERIFY(!tw::trim(messages, tw::RetainedEdge::Oldest, 2, tw::unboundedUtf8Bytes).changed);
        QCOMPARE(messages.size(), 2);
        const auto result = tw::trim(messages, tw::RetainedEdge::Oldest, 1, tw::unboundedUtf8Bytes);
        QVERIFY(result.changed && result.removedNewer);
        QCOMPARE(messages.constFirst().value("messageKey").toString(), QStringLiteral("a"));
    }

    void byteCutNeverReachesTheProtectedTurn() {
        // Ten 3 MB turns: the reader's turn 6 must stay whatever the bound needs.
        QVector<QVariantMap> messages;
        for (int turn = 0; turn < 10; ++turn)
            messages.append(
                row(QString::number(turn), QStringLiteral("t%1").arg(turn), 3 * 1000 * 1000));
        auto cut = tw::byteCut(messages, 6, 7, true);
        QCOMPARE(cut.fromOldest + cut.fromNewest, 5);
        QVERIFY(cut.fromOldest <= 6 && cut.fromNewest <= 3);
        // Newer rows are where a running turn appends: only the older side shrinks.
        cut = tw::byteCut(messages, 6, 7, false);
        QCOMPARE(cut.fromOldest, 5);
        QCOMPARE(cut.fromNewest, 0);
        // A reader on the oldest turn of a running window leaves nothing to evict.
        cut = tw::byteCut(messages, 0, 1, false);
        QCOMPARE(cut.fromOldest, 0);
        QCOMPARE(cut.fromNewest, 0);
    }

    void byteLimitCanBeRaisedWhileScrolling() {
        // Two turns of 10 MB each: the idle bound keeps one, a raised bound keeps both.
        QVector<QVariantMap> messages;
        for (int turn = 0; turn < 2; ++turn)
            for (int i = 0; i < 10; ++i)
                messages.append(row(QStringLiteral("t%1-%2").arg(turn).arg(i),
                                    QStringLiteral("turn-%1").arg(turn), 1000 * 1000));
        auto raised = messages;
        QVERIFY(!tw::trim(raised, tw::RetainedEdge::Newest, tw::scrollingMessageLimit,
                          tw::scrollingUtf8Bytes)
                     .changed);
        QCOMPARE(raised.size(), 20);
        QVERIFY(tw::trim(messages, tw::RetainedEdge::Newest).removedOlder);
        QCOMPARE(messages.size(), 10);
    }

    void singleIndivisibleMessageIsKept() {
        QVector<QVariantMap> messages{row("big", {}, tw::maximumUtf8Bytes + 10)};
        QVERIFY(!tw::trim(messages, tw::RetainedEdge::Newest).changed);
        QCOMPARE(messages.size(), 1);
    }
};

QTEST_MAIN(AgentTranscriptWindowBudgetTest)
#include "AgentTranscriptWindowBudgetTest.moc"
