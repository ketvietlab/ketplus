#include "AgentWorkspaceViewTest.h"
#include "agent/AgentMessageModel.h"
#include "agent/AgentWorkspaceView.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickView>
#include <QSignalSpy>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <ctime>

using ketplus::AgentWorkspaceView;

namespace {
QVariantMap streamed(const QString& body, const QString& key = "first") {
    return {{"kind", "assistant"},
            {"body", body},
            {"meta", QStringLiteral("Codex · streaming")},
            {"messageKey", key}};
}
} // namespace

namespace perf {
QString codeHeavyBody(qsizetype targetCharacters) {
    QString body;
    int section = 0;
    while (body.size() < targetCharacters) {
        body += QStringLiteral("## Section %1\n\nParagraph with **bold**, `code` and "
                               "[link](src/main.cpp) number %1.\n\n")
                    .arg(section);
        body += QStringLiteral("```cpp\n");
        for (int line = 0; line < 12; ++line)
            body += QStringLiteral("int value%1_%2 = compute(%2); // comment\n\n")
                        .arg(section)
                        .arg(line);
        body += QStringLiteral("```\n\n");
        ++section;
    }
    return body;
}

QVariantList history(int rows) {
    QVariantList result;
    for (int row = 0; row < rows; ++row) {
        if (row % 5 == 2)
            result.append(QVariantMap{{"kind", "tool"},
                                      {"body", QStringLiteral("tool output %1").arg(row)},
                                      {"messageKey", QStringLiteral("h%1").arg(row)}});
        else
            result.append(QVariantMap{
                {"kind", row % 2 ? "assistant" : "user"},
                {"body", QStringLiteral("History row %1 with some text.\n\nMore.").arg(row)},
                {"messageKey", QStringLiteral("h%1").arg(row)}});
    }
    return result;
}

struct Counters {
    int dataChanged{0};
    int rowsInserted{0};
    int rowsRemoved{0};
    int modelReset{0};
};

void count(QAbstractItemModel* model, Counters* counters, QObject* context) {
    QObject::connect(model, &QAbstractItemModel::dataChanged, context,
                     [counters] { ++counters->dataChanged; });
    QObject::connect(model, &QAbstractItemModel::rowsInserted, context,
                     [counters] { ++counters->rowsInserted; });
    QObject::connect(model, &QAbstractItemModel::rowsRemoved, context,
                     [counters] { ++counters->rowsRemoved; });
    QObject::connect(model, &QAbstractItemModel::modelReset, context,
                     [counters] { ++counters->modelReset; });
}
} // namespace perf

class AgentWorkspaceStreamingTest final : public QObject {
    Q_OBJECT
  private slots:
    void incrementalStreamingLeavesCommittedBlocksUntouched() {
        const QString final = perf::codeHeavyBody(70 * 1024);
        auto items = perf::history(20);
        items.append(streamed("", "stream"));
        items.append(QVariantMap{{"kind", "user"}, {"body", "tail"}, {"messageKey", "after"}});
        ketplus::AgentMessageModel model;
        model.reset(items);
        const int streamRow = 20;
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        const int steps = 400;
        for (int step = 1; step <= steps; ++step) {
            const int firstRendered = model.indexOfKey("stream");
            int blocks = 1;
            while (firstRendered + blocks < model.rowCount() &&
                   model.get(firstRendered + blocks).value("chunkContinuation").toBool())
                ++blocks;
            changed.clear();
            model.set(streamRow, streamed(final.left(final.size() * step / steps), "stream"));
            for (const auto& args : changed) {
                const int row = args.at(0).value<QModelIndex>().row();
                // Only the previous tail block (or later) may change.
                QVERIFY2(row >= firstRendered + blocks - 1,
                         qPrintable(QStringLiteral("committed row %1 changed").arg(row)));
            }
            QCOMPARE(model.indexOfKey("after"), model.rowCount() - 1);
        }
        QCOMPARE(resets.count(), 0);
        ketplus::AgentMessageModel expected;
        QVariantList source;
        for (int i = 0; i < model.sourceCount(); ++i)
            source.append(model.source(i));
        expected.reset(source);
        QCOMPARE(model.rowCount(), expected.rowCount());
        QString joined;
        for (int row = 0; row < model.rowCount(); ++row) {
            QCOMPARE(model.get(row), expected.get(row));
            QCOMPARE(model.indexOfKey(model.get(row).value("messageKey").toString()), row);
            if (model.get(row).value("messageKey").toString().startsWith("stream"))
                joined += model.get(row).value("body").toString();
        }
        QCOMPARE(joined, final);
        // A non-append rewrite still renders correctly (full path).
        model.set(streamRow, streamed(final.mid(5000), "stream"));
        QVariantList rewritten;
        for (int i = 0; i < model.sourceCount(); ++i)
            rewritten.append(model.source(i));
        expected.reset(rewritten);
        QCOMPARE(model.rowCount(), expected.rowCount());
        for (int row = 0; row < model.rowCount(); ++row)
            QCOMPARE(model.get(row), expected.get(row));
    }

    void indexesTrackStructuralChangesAndTrimming() {
        const QString big = perf::codeHeavyBody(40 * 1024);
        QVariantList items;
        for (int row = 0; row < 12; ++row)
            items.append(QVariantMap{{"kind", "assistant"},
                                     {"body", row % 3 == 0 ? big : QStringLiteral("s%1").arg(row)},
                                     {"messageKey", QStringLiteral("k%1").arg(row)}});
        ketplus::AgentMessageModel model;
        model.reset(items.mid(4));
        model.prepend(items.mid(0, 4));
        const qsizetype bytesBefore = model.estimatedTextBytes();
        model.append(QVariantMap{{"kind", "user"}, {"body", "new"}, {"messageKey", "k12"}});
        QVERIFY(model.estimatedTextBytes() > bytesBefore);
        // Duplicate key updates the existing source row.
        model.append(QVariantMap{{"kind", "assistant"}, {"body", "updated"}, {"messageKey", "k6"}});
        QCOMPARE(model.sourceCount(), 13);
        QCOMPARE(model.get(model.indexOfKey("k6")).value("body").toString(), QString("updated"));
        QVariantList current;
        for (int i = 0; i < model.sourceCount(); ++i)
            current.append(model.source(i));
        const auto checkMatchesReset = [&model](const QVariantList& source) {
            ketplus::AgentMessageModel expected;
            expected.reset(source);
            QCOMPARE(model.rowCount(), expected.rowCount());
            for (int row = 0; row < model.rowCount(); ++row) {
                QCOMPARE(model.get(row).value("messageKey"), expected.get(row).value("messageKey"));
                QCOMPARE(model.get(row).value("body"), expected.get(row).value("body"));
                QCOMPARE(model.indexOfKey(model.get(row).value("messageKey").toString()), row);
            }
        };
        checkMatchesReset(current);
        // Trim the tail with chunked rows retained before the cut.
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        const auto trimmed = current.mid(0, 7);
        model.replaceWindow(trimmed);
        QCOMPARE(resets.count(), 0);
        QCOMPARE(model.sourceCount(), 7);
        checkMatchesReset(trimmed);
        QVERIFY(model.estimatedTextBytes() < bytesBefore);
        // Idle trim cuts both edges around the reader in one window, still without a reset.
        const auto centred = trimmed.mid(2, 3);
        model.replaceWindow(centred);
        QCOMPARE(resets.count(), 0);
        QCOMPARE(model.sourceCount(), 3);
        checkMatchesReset(centred);
        model.replaceWindow(trimmed);
        checkMatchesReset(trimmed);
        // Structured roles are exposed without JSON encoding.
        model.append(QVariantMap{{"kind", "user"},
                                 {"body", "img"},
                                 {"images", QVariantList{"file:///a.png", " "}},
                                 {"messageKey", "img"}});
        const auto image = model.get(model.indexOfKey("img"));
        QCOMPARE(image.value("imageSources").toList(), QVariantList{"file:///a.png"});
        QCOMPARE(image.value("questionAnswerData").toMap(), QVariantMap{});
    }

    void transcriptPerfBenchmark_data() {
        QTest::addColumn<int>("flushEvery");
        QTest::newRow("flush-per-20-deltas") << 20;
        QTest::newRow("flush-per-delta") << 1;
    }

    void transcriptPerfBenchmark() {
        if (!qEnvironmentVariableIsSet("KETPLUS_RUN_PERF_BENCHMARK"))
            QSKIP("Opt-in transcript model benchmark");
        QFETCH(int, flushEvery);
        const int historyRows = 300;
        const int deltas = 2000;
        const QString final = perf::codeHeavyBody(90 * 1024);
        auto items = perf::history(historyRows);
        items.append(streamed("", "stream"));
        ketplus::AgentMessageModel model;
        model.reset(items);
        perf::Counters counters;
        perf::count(&model, &counters, &model);
        QElapsedTimer total;
        QElapsedTimer flush;
        qint64 maxNs = 0;
        qint64 sumNs = 0;
        int flushes = 0;
        total.start();
        for (int delta = 1; delta <= deltas; ++delta) {
            if (delta % flushEvery != 0 && delta != deltas)
                continue;
            const auto body = final.left(final.size() * delta / deltas);
            flush.start();
            model.set(historyRows, streamed(body, "stream"));
            const qint64 ns = flush.nsecsElapsed();
            maxNs = std::max(maxNs, ns);
            sumNs += ns;
            ++flushes;
        }
        const double totalMs = total.nsecsElapsed() / 1e6;
        model.set(historyRows, QVariantMap{{"kind", "assistant"},
                                           {"body", final},
                                           {"meta", "Codex"},
                                           {"messageKey", "stream"}});
        ketplus::AgentMessageModel expected;
        QVariantList source;
        for (int i = 0; i < model.sourceCount(); ++i)
            source.append(model.source(i));
        expected.reset(source);
        QCOMPARE(model.rowCount(), expected.rowCount());
        for (int row = 0; row < model.rowCount(); ++row) {
            auto a = model.get(row);
            auto b = expected.get(row);
            QCOMPARE(a, b);
        }

        // Warm conversation switch with 8 models.
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 800);
        view.show();
        QList<QVariantList> scopes;
        for (int scope = 0; scope < 8; ++scope) {
            auto scoped = perf::history(historyRows);
            scoped.append(QVariantMap{{"kind", "assistant"},
                                      {"body", final},
                                      {"messageKey", QStringLiteral("big%1").arg(scope)}});
            scopes.append(scoped);
            view.resetMessages(scoped, QStringLiteral("scope%1").arg(scope));
        }
        QTest::qWait(50);
        QElapsedTimer switching;
        qint64 switchMaxNs = 0;
        qint64 switchSumNs = 0;
        const int switches = 64;
        for (int i = 0; i < switches; ++i) {
            const int scope = i % 8;
            switching.start();
            view.resetMessages(scopes.at(scope), QStringLiteral("scope%1").arg(scope));
            const qint64 ns = switching.nsecsElapsed();
            switchMaxNs = std::max(switchMaxNs, ns);
            switchSumNs += ns;
        }
        QElapsedTimer compare;
        compare.start();
        QVERIFY(!view.messageModel()->resetIfChanged(scopes.at((switches - 1) % 8)));
        const double compareMs = compare.nsecsElapsed() / 1e6;
        qInfo().noquote() << "PERF_SWITCH_COMPARE_MS" << compareMs;

        // Trim a window: drop the tail half of a transcript holding large rows.
        QVariantList trimSource;
        for (int row = 0; row < historyRows; ++row)
            trimSource.append(QVariantMap{{"kind", "assistant"},
                                          {"body", row % 10 == 0 ? final : QString("short")},
                                          {"messageKey", QStringLiteral("t%1").arg(row)}});
        ketplus::AgentMessageModel trim;
        qint64 trimNs = 0;
        const int trims = 10;
        for (int i = 0; i < trims; ++i) {
            trim.reset(trimSource);
            QElapsedTimer timer;
            timer.start();
            trim.replaceWindow(trimSource.mid(0, historyRows / 2));
            trimNs += timer.nsecsElapsed();
        }
        QCOMPARE(trim.sourceCount(), historyRows / 2);

        qInfo().noquote()
            << "PERF_METRIC"
            << QJsonDocument(
                   QJsonObject{{"sample", QTest::currentDataTag()},
                               {"flushes", flushes},
                               {"flushAvgMs", sumNs / 1e6 / flushes},
                               {"flushMaxMs", maxNs / 1e6},
                               {"streamTotalMs", totalMs},
                               {"dataChanged", counters.dataChanged},
                               {"rowsInserted", counters.rowsInserted},
                               {"rowsRemoved", counters.rowsRemoved},
                               {"modelReset", counters.modelReset},
                               {"switchAvgMs", switchSumNs / 1e6 / switches},
                               {"switchMaxMs", switchMaxNs / 1e6},
                               {"trimAvgMs", trimNs / 1e6 / trims}})
                   .toJson(QJsonDocument::Compact);
    }

    void streamedBodyCrossingChunkBoundariesDoesNotResetModel() {
        const auto paragraphs = [](int count, const QString& tag) {
            QString body;
            for (int i = 0; i < count; ++i)
                body += QStringLiteral("%1 paragraph %2 ").arg(tag).arg(i).repeated(8) + "\n\n";
            return body;
        };
        ketplus::AgentMessageModel model;
        model.reset({QVariantMap{{"kind", "user"}, {"body", "hi"}, {"messageKey", "user-1"}},
                     streamed(paragraphs(400, "a"), "assistant-1"),
                     QVariantMap{{"kind", "user"}, {"body", "after"}, {"messageKey", "user-2"}}});
        const int initialRows = model.rowCount();
        QVERIFY(initialRows > 3);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserts(&model, &QAbstractItemModel::rowsInserted);

        // Prefix the body so every continuation offset (and key) shifts, then grow it.
        const QString shifted = QString(777, QLatin1Char('x')) + "\n\n" + paragraphs(600, "b");
        model.set(1, streamed(shifted, "assistant-1"));
        QCOMPARE(resets.count(), 0);
        QVERIFY(inserts.count() > 0);
        QCOMPARE(model.get(0).value("messageKey").toString(), QStringLiteral("user-1"));
        QCOMPARE(model.get(model.rowCount() - 1).value("messageKey").toString(),
                 QStringLiteral("user-2"));
        QCOMPARE(model.indexOfKey("user-2"), model.rowCount() - 1);

        ketplus::AgentMessageModel expected;
        QVariantList source;
        for (int i = 0; i < model.sourceCount(); ++i)
            source.append(model.source(i));
        expected.reset(source);
        QCOMPARE(model.rowCount(), expected.rowCount());
        for (int row = 0; row < model.rowCount(); ++row)
            QCOMPARE(model.get(row), expected.get(row));

        model.set(1, streamed(paragraphs(10, "c"), "assistant-1"));
        QCOMPARE(resets.count(), 0);
        QCOMPARE(model.get(model.rowCount() - 1).value("messageKey").toString(),
                 QStringLiteral("user-2"));
    }
    void streamBenchmark_data() {
        QTest::addColumn<int>("historyRows");
        QTest::addColumn<bool>("coalesced");
        if (!qEnvironmentVariableIsSet("KETPLUS_RUN_STREAM_BENCHMARK")) {
            QTest::newRow("disabled") << 0 << true;
            return;
        }
        for (int sample = 0; sample < 3; ++sample)
            for (const int rows : {1000, 10000})
                for (const bool batch : {false, true})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(rows).arg(batch).arg(sample)))
                        << rows << batch;
    }

    void streamBenchmark() {
        if (!qEnvironmentVariableIsSet("KETPLUS_RUN_STREAM_BENCHMARK"))
            QSKIP("Opt-in synthetic Release render benchmark; no provider calls");
        QFETCH(int, historyRows);
        QFETCH(bool, coalesced);
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 800);
        view.show();
        QVariantList history;
        for (int row = 0; row < historyRows; ++row)
            history.append(QVariantMap{{"kind", "user"},
                                       {"body", "Synthetic history"},
                                       {"messageKey", QString::number(row)}});
        history.append(streamed(""));
        view.resetMessages(history, "benchmark");
        QTest::qWait(250);
        int publications = 0;
        qint64 publishedCharacters = 0;
        connect(view.messageModel(), &QAbstractItemModel::dataChanged, &view,
                [&](const QModelIndex& index) {
                    ++publications;
                    publishedCharacters +=
                        view.messageModel()->get(index.row()).value("body").toString().size();
                });
        const QString final =
            QString("Paragraph with **bold** and `code`, plus [link](src/main.cpp).\n\n")
                .repeated(130);
        QElapsedTimer wall;
        wall.start();
        const auto cpuStart = std::clock();
        for (int delta = 1; delta <= 300; ++delta) {
            const auto item = streamed(final.left(final.size() * delta / 300));
            if (coalesced) {
                view.updateMessage(historyRows, item);
            } else {
                // Same binary/renderer, original uncoalesced presentation path.
                // This is a controlled path comparison, not an old-build claim.
                QVERIFY(QMetaObject::invokeMethod(
                    view.quickView()->rootObject(), "updateSessionMessage",
                    Q_ARG(QVariant, historyRows), Q_ARG(QVariant, item)));
            }
            QTest::qWait(20);
        }
        view.setSessionState("Done", "Ready", false);
        QTest::qWait(50);
        QCOMPARE(view.messageModel()->get(historyRows).value("body").toString(), final);
        const double cpuMs = 1000.0 * (std::clock() - cpuStart) / CLOCKS_PER_SEC;
        qInfo().noquote() << "STREAM_METRIC"
                          << QJsonDocument(
                                 QJsonObject{{"sample", QTest::currentDataTag()},
                                             {"historyRows", historyRows},
                                             {"coalesced", coalesced},
                                             {"wallMs", double(wall.elapsed())},
                                             {"cpuMs", cpuMs},
                                             {"publications", publications},
                                             {"publishedCharacters", double(publishedCharacters)},
                                             {"finalCharacters", double(final.size())},
                                             {"deltas", 300},
                                             {"qt", QT_VERSION_STR},
                                             {"os", QSysInfo::prettyProductName()},
                                             {"architecture", QSysInfo::currentCpuArchitecture()},
                                             {"platform", QGuiApplication::platformName()}})
                                 .toJson(QJsonDocument::Compact);
        if (coalesced)
            QVERIFY(publications < 100);
        else
            QCOMPARE(publications, 300);
    }

    void burstIsCoalescedAndCompletionWins() {
        AgentWorkspaceView view;
        view.resetMessages({streamed("")}, "a");
        QSignalSpy changed(view.messageModel(), &QAbstractItemModel::dataChanged);
        QString body;
        for (int i = 0; i < 1669; ++i) {
            body += "text ";
            view.updateMessage(0, streamed(body));
        }
        QCOMPARE(changed.size(), 0);
        QTRY_COMPARE(view.messageModel()->get(0).value("body").toString(), body);
        QCOMPARE(changed.size(), 1);
        view.updateMessage(0, streamed(body + " pending"));
        auto final = streamed("Authoritative **final** text");
        final["meta"] = "Codex";
        view.updateMessage(0, final);
        QCOMPARE(view.messageModel()->get(0).value("body"), final.value("body"));
        QTest::qWait(180);
        QCOMPARE(view.messageModel()->get(0).value("body"), final.value("body"));
        QCOMPARE(changed.size(), 2);
    }

    void sustainedStreamMakesProgress() {
        AgentWorkspaceView view;
        view.resetMessages({streamed("")}, "a");
        QSignalSpy changed(view.messageModel(), &QAbstractItemModel::dataChanged);
        QString body;
        for (int i = 0; i < 25; ++i) {
            body += "delta ";
            view.updateMessage(0, streamed(body));
            QTest::qWait(20);
        }
        QVERIFY(changed.size() >= 2);
        QVERIFY(changed.size() < 15);
        view.setSessionState("Interrupted", "Stopped", false);
        QCOMPARE(view.messageModel()->get(0).value("body").toString(), body);
        const auto publications = changed.size();
        QTest::qWait(180);
        QCOMPARE(changed.size(), publications);
    }

    void partialSourceBatchesDeltasButPublishesAnExplicitPrefixImmediately() {
        AgentWorkspaceView view;
        auto item = streamed("suffix");
        item.insert("meta", "Codex · Earlier text not loaded");
        item.insert("partialBody", true);
        view.resetMessages({item}, "partial");
        QSignalSpy changed(view.messageModel(), &QAbstractItemModel::dataChanged);
        for (int delta = 0; delta < 20; ++delta) {
            item.insert("body", item.value("body").toString() + " tail");
            view.updateMessage(0, item);
        }
        QCOMPARE(changed.size(), 0);
        QTRY_COMPARE(changed.size(), 1);
        item.insert("body", "prefix " + item.value("body").toString());
        item.insert("bodyPrefixCharacters", 7);
        view.updateMessage(0, item);
        QCOMPARE(changed.size(), 2);
        QCOMPARE(view.messageModel()->get(0).value("body"), item.value("body"));
    }

    void switchingAndPrependingFencePendingRows() {
        AgentWorkspaceView view;
        view.resetMessages({streamed("a")}, "a");
        auto* first = view.messageModel();
        view.updateMessage(0, streamed("a updated"));
        view.resetMessages({streamed("b", "second")}, "b");
        QVERIFY(first != view.messageModel());
        QCOMPARE(first->get(0).value("body").toString(), "a updated");
        view.updateMessage(0, streamed("b updated", "second"));
        view.prependMessages({streamed("older", "older")});
        QCOMPARE(view.messageModel()->get(0).value("body").toString(), "older");
        QCOMPARE(view.messageModel()->get(1).value("body").toString(), "b updated");
        view.resetMessages({streamed("a updated")}, "a");
        QCOMPARE(view.messageModel(), first);
        QTest::qWait(180);
        QCOMPARE(first->count(), 1);
        QCOMPARE(first->get(0).value("body").toString(), "a updated");
    }

    void appendAndMultipleItemsPreserveOrdering() {
        AgentWorkspaceView view;
        view.resetMessages({streamed(""), streamed("", "second")}, "a");
        view.updateMessage(1, streamed("second body", "second"));
        view.updateMessage(0, streamed("first body"));
        view.appendMessage({{"kind", "tool"}, {"body", "Run command"}});
        QCOMPARE(view.messageModel()->get(0).value("body").toString(), "first body");
        QCOMPARE(view.messageModel()->get(1).value("body").toString(), "second body");
        QCOMPARE(view.messageModel()->count(), 3);
    }

    void samePathGitRefreshDoesNotScanSkills() {
        QTemporaryDir first;
        QTemporaryDir second;
        QVERIFY(first.isValid() && second.isValid());
        AgentWorkspaceView view;
        view.setWorktrees({}, first.path(), {});
        QSignalSpy skills(&view, &AgentWorkspaceView::skillsChanged);
        QSignalSpy paths(&view, &AgentWorkspaceView::activePathChanged);
        for (int i = 0; i < 50; ++i)
            view.setWorktrees({}, first.path(),
                              i % 2 ? QSet<QString>{first.path()} : QSet<QString>{});
        QCOMPARE(skills.size(), 0);
        QCOMPARE(paths.size(), 0);
        view.refreshSkills();
        QCOMPARE(skills.size(), 1);
        view.setWorktrees({}, second.path(), {});
        QTRY_COMPARE(skills.size(), 2);
        QCOMPARE(paths.size(), 1);
    }

    void bufferedStreamPreservesReaderAndLatest() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        QString body = QString("Paragraph with **bold** and `code`.\n\n").repeated(150);
        view.resetMessages({streamed(body)}, "reading");
        auto* root = view.quickView()->rootObject();
        QVERIFY(root);
        auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        QTRY_COMPARE(transcript->opacity(), 1.0);
        QTest::qWait(100);
        transcript->setProperty("followTail", false);
        transcript->setProperty("contentY", transcript->property("originY").toDouble() + 200);
        QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
        QTest::qWait(100);
        const double before = transcript->property("contentY").toDouble();
        for (int delta = 0; delta < 30; ++delta) {
            body += "More **streamed** text.\n\n";
            view.updateMessage(0, streamed(body));
            QTest::qWait(10);
        }
        view.setSessionState("Done", "Ready", false);
        QTest::qWait(100);
        QCOMPARE(view.messageModel()->get(0).value("body").toString(), body);
        QVERIFY(!transcript->property("followTail").toBool());
        QVERIFY(qAbs(transcript->property("contentY").toDouble() - before) < 1);
        auto* latest = root->findChild<QQuickItem*>("jumpToLatestButton");
        QVERIFY(latest);
        QVERIFY(QMetaObject::invokeMethod(latest, "clicked"));
        QTRY_VERIFY(transcript->property("followTail").toBool());
        QTRY_VERIFY(transcript->property("atYEnd").toBool());
        body += "The tail continues.\n\n";
        view.updateMessage(0, streamed(body));
        QTRY_COMPARE(view.messageModel()->get(0).value("body").toString(), body);
        QTRY_VERIFY(transcript->property("atYEnd").toBool());
    }
};

QTEST_MAIN(AgentWorkspaceStreamingTest)
#include "AgentWorkspaceStreamingTest.moc"
