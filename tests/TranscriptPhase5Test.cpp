#include "AgentWorkspaceViewTest.h"

#include "agent/AgentMessageFormatter.h"
#include "agent/AgentMessageModel.h"
#include "agent/AgentWorkspaceView.h"

#include <QAccessible>
#include <QClipboard>
#include <QGuiApplication>
#include <QJSValue>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextList>
#include <QTimer>
#include <QtTest>

using namespace agent_workspace_test;

namespace {

QVariantMap readRow(const QString& key, const QString& command, const bool running = false,
                    const QString& turn = QStringLiteral("turn-1")) {
    return {
        {"messageKey", key},     {"kind", "tool"},  {"category", "activity"},
        {"subtype", "fileRead"}, {"turnId", turn},  {"title", command},
        {"command", command},    {"body", command}, {"status", running ? "running" : "completed"},
        {"running", running},    {"filePath", ""}};
}

QVariantMap textRow(const QString& key, const QString& kind, const QString& body,
                    const QString& turn = QStringLiteral("turn-1")) {
    return {{"messageKey", key},
            {"kind", kind},
            {"meta", kind == QStringLiteral("user") ? "You" : "Codex"},
            {"body", body},
            {"turnId", turn},
            {"filePath", ""}};
}

QQuickItem* delegateAt(QQuickItem* transcript, const int index) {
    QQuickItem* item = nullptr;
    QMetaObject::invokeMethod(transcript, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, item),
                              Q_ARG(int, index));
    return item;
}

// Collapsing a group removes rows, so a row number is not a stable way to name
// the reader's row across a toggle. Follow the key instead.
QQuickItem* delegateForKey(QQuickItem* transcript, const QString& key) {
    auto* model = transcript->property("messageModel").value<ketplus::AgentMessageModel*>();
    if (model == nullptr)
        return nullptr;
    const int row = model->indexOfKey(key);
    return row >= 0 ? delegateAt(transcript, row) : nullptr;
}

QVariantMap roles(ketplus::AgentMessageModel* model, const int index) { return model->get(index); }
// Rows withheld while their group is collapsed are still rendered and still
// grouped; they are addressed through the rendered list, not the model rows.
QVariantMap rendered(ketplus::AgentMessageModel* model, const int row) {
    return model->renderedRow(row);
}

QQuickItem* childNamed(QQuickItem* item, const QString& name) {
    return item ? findVisualItem(item, name) : nullptr;
}

bool clickButton(QQuickItem* button) {
    return button && QMetaObject::invokeMethod(button, "clicked");
}

QVariant call(QObject* object, const char* method, const QVariant& argument = {}) {
    QVariant result;
    if (argument.isValid())
        QMetaObject::invokeMethod(object, method, Q_RETURN_ARG(QVariant, result),
                                  Q_ARG(QVariant, argument));
    else
        QMetaObject::invokeMethod(object, method, Q_RETURN_ARG(QVariant, result));
    return result;
}

bool groupExpanded(QObject* transcript, const QString& key) {
    QVariant expanded;
    QMetaObject::invokeMethod(transcript, "groupExpanded", Q_RETURN_ARG(QVariant, expanded),
                              Q_ARG(QVariant, key));
    return expanded.toBool();
}

bool hasExplicitExpansion(QObject* transcript, const QString& key) {
    auto* model = transcript->property("messageModel").value<ketplus::AgentMessageModel*>();
    return model != nullptr && model->hasExplicitExpansion(key);
}

// Runs Qt.callLater callbacks and zero-interval timers queued so far (no fixed sleep).
void flushDeferredCalls() {
    for (int turn = 0; turn < 3; ++turn) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}

void pressKey(QObject* target, const int key, const Qt::KeyboardModifiers modifiers = {}) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QCoreApplication::sendEvent(target, &event);
}

int countObjects(const QObject* object, int* buttons = nullptr) {
    int total = 1;
    if (buttons != nullptr && object->inherits("QQuickAbstractButton"))
        ++*buttons;
    for (const QObject* child : object->children())
        total += countObjects(child, buttons);
    return total;
}

// Leaves the tail and reads from `index` (the reader no longer follows new rows).
void readFrom(QQuickItem* transcript, const int index, const double offset = 0) {
    QMetaObject::invokeMethod(transcript, "positionViewAtIndex", Q_ARG(int, index), Q_ARG(int, 1));
    if (offset != 0)
        transcript->setProperty("contentY", transcript->property("contentY").toDouble() + offset);
    QMetaObject::invokeMethod(transcript, "movementEnded");
}

} // namespace

void AgentWorkspaceViewTest::activityGroupRolesUpdateIncrementally() {
    ketplus::AgentMessageModel model;
    model.reset({textRow("u", "user", "Run things"), readRow("c1", "ls"), readRow("c2", "pwd")});
    // Collapsed: the group is one row, so the user row and the leader are all there is.
    QCOMPARE(model.count(), 2);
    QCOMPARE(model.renderedCount(), 3);
    QCOMPARE(roles(&model, 1).value("groupRole").toString(), QStringLiteral("leader"));
    QCOMPARE(roles(&model, 1).value("groupSummary").toString(), QStringLiteral("Read 2 files"));
    QCOMPARE(rendered(&model, 2).value("groupRole").toString(), QStringLiteral("member"));

    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy inserts(&model, &QAbstractItemModel::rowsInserted);
    model.append(readRow("c3", "make", true));
    QCOMPARE(resets.size(), 0);
    // A row joining a collapsed group adds no row: the reader sees the summary change.
    QCOMPARE(inserts.size(), 0);
    QCOMPARE(model.count(), 2);
    QSet<int> changedRows;
    for (const auto& change : changes)
        changedRows.insert(change.at(0).toModelIndex().row());
    // The leader's summary and running flag, never the user row.
    QCOMPARE(changedRows, QSet<int>({1}));
    QCOMPARE(roles(&model, 1).value("groupSize").toInt(), 3);
    QVERIFY(roles(&model, 1).value("groupRunning").toBool());

    changes.clear();
    // A delta for a member of a collapsed group repaints the leader.
    model.set(3, readRow("c3", "make"));
    QCOMPARE(resets.size(), 0);
    QVERIFY(!roles(&model, 1).value("groupRunning").toBool());
    for (const auto& change : changes)
        QCOMPARE(change.at(0).toModelIndex().row(), 1);

    // Unrelated streaming text updates publish no group role changes.
    model.append(textRow("a", "assistant", "Done"));
    changes.clear();
    model.set(4, textRow("a", "assistant", "Done, all good"));
    for (const auto& change : changes)
        QCOMPARE(change.at(0).toModelIndex().row(), 2);
    QCOMPARE(resets.size(), 0);

    model.prepend(
        {readRow("p1", "git status", false, "turn-0"), readRow("p2", "git diff", false, "turn-0")});
    QCOMPARE(roles(&model, 0).value("groupRole").toString(), QStringLiteral("leader"));
    QCOMPARE(rendered(&model, 3).value("groupRole").toString(), QStringLiteral("leader"));
}

void AgentWorkspaceViewTest::activityGroupKeyStableAcrossPagingAndTrims() {
    ketplus::AgentMessageModel model;
    model.reset({readRow("c2", "b"), readRow("c3", "c"), readRow("c4", "d"),
                 textRow("a", "assistant", "Done")});
    QCOMPARE(model.get(0).value("groupKey").toString(), QStringLiteral("c2"));
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);

    // An older page ending with same-turn activity takes over the leader, not the identity.
    model.prepend({textRow("u", "user", "Go"), readRow("c1", "a")});
    QCOMPARE(model.get(1).value("groupRole").toString(), QStringLiteral("leader"));
    // The group is one row; its members keep the identity in the rendered list.
    for (int row = 1; row <= 4; ++row)
        QCOMPARE(model.renderedRow(row).value("groupKey").toString(), QStringLiteral("c2"));
    QCOMPARE(model.groupLeaderIndex(1), 1);
    QCOMPARE(model.groupLeaderIndex(0), -1);

    // Newer page evicting the oldest rows, leader included (front trim).
    QVariantList window{readRow("c3", "c"), readRow("c4", "d"), textRow("a", "assistant", "Done"),
                        textRow("a2", "assistant", "More")};
    model.replaceWindow(window);
    // c3 leads the group and c4 is withheld inside it: 3 rows for 4 rendered.
    QCOMPARE(model.count(), 3);
    QCOMPARE(model.renderedCount(), 4);
    QCOMPARE(model.get(0).value("groupRole").toString(), QStringLiteral("leader"));
    QCOMPARE(model.get(0).value("groupKey").toString(), QStringLiteral("c2"));
    // Row 1 is the assistant message after the group, not c4: c4 is withheld, so
    // no model row addresses it and nothing outside the group reports a leader.
    QCOMPARE(model.groupLeaderIndex(0), 0);
    QCOMPARE(model.groupLeaderIndex(1), -1);

    // Older page trimming the newest rows (back trim) and extending the group again.
    model.replaceWindow({readRow("c0", "z"), readRow("c3", "c"), readRow("c4", "d")});
    // All three are one collapsed group, so the window is a single row.
    QCOMPARE(model.count(), 1);
    QCOMPARE(model.renderedCount(), 3);
    QCOMPARE(model.hiddenRowCount(), 2);
    QCOMPARE(model.get(0).value("groupSize").toInt(), 3);
    for (int row = 0; row < 3; ++row)
        QCOMPARE(model.renderedRow(row).value("groupKey").toString(), QStringLiteral("c2"));
    QCOMPARE(model.activityGroupKeys(), QStringList({QStringLiteral("c2")}));
    QCOMPARE(resets.size(), 0);
}

void AgentWorkspaceViewTest::anchorKeysResolveToVisibleRows() {
    ketplus::AgentMessageModel model;
    model.reset({readRow("c2", "b"), readRow("c3", "c"), textRow("a", "assistant", "Done")});
    // A row the view lays out resolves to itself; a withheld member to its group row.
    QCOMPARE(model.visibleKeyFor("a"), QStringLiteral("a"));
    QCOMPARE(model.visibleKeyFor("c3"), QStringLiteral("c2"));
    QCOMPARE(model.visibleKeyFor("gone"), QString{});

    // An older page takes over the leader: the old leader is withheld now, and a
    // reader anchored on it is on the group row the new leader shows.
    model.prepend({textRow("u", "user", "Go"), readRow("c1", "a")});
    QCOMPARE(model.indexOfKey("c2"), -1);
    QCOMPARE(model.visibleKeyFor("c2"), QStringLiteral("c1"));

    // Blocks of a long answer: a renamed block resolves to the block of the same
    // message nearest its old offset.
    QString body;
    while (body.size() < 40 * 1024)
        body += QStringLiteral("A paragraph of the long answer, long enough to wrap.\n\n");
    model.reset({textRow("long", "assistant", body)});
    QVERIFY(model.count() > 2);
    const QString second = model.get(1).value("messageKey").toString();
    const QString last = model.get(model.count() - 1).value("messageKey").toString();
    QVERIFY(second.startsWith(QStringLiteral("long:block:")));
    QCOMPARE(model.visibleKeyFor(second), second);
    QCOMPARE(model.visibleKeyFor(QStringLiteral("long:block:1")), QStringLiteral("long"));
    QCOMPARE(model.visibleKeyFor(QStringLiteral("long:block:999999999")), last);
}

void AgentWorkspaceViewTest::readerHeldWhenPrependTakesOverGroupLeader() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QVariantList rows;
    for (int index = 0; index < 4; ++index)
        rows.append(
            readRow(QStringLiteral("c%1").arg(index), QStringLiteral("step %1").arg(index)));
    for (int index = 0; index < 40; ++index)
        rows.append(textRow(QStringLiteral("a%1").arg(index), "assistant",
                            QStringLiteral("Paragraph %1 of the answer.").arg(index)));
    view.resetMessages(rows, "workspace:leader");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    auto* model = transcript->property("messageModel").value<ketplus::AgentMessageModel*>();
    QVERIFY(model);
    QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtBeginning"));
    transcript->setProperty("followTail", false);
    QTRY_VERIFY(delegateForKey(transcript, "c0"));
    flushDeferredCalls();
    auto* geometry = model->geometry();
    const auto screenTop = [&](const QString& key) {
        return geometry->top(geometry->rowForKey(key)) -
               transcript->property("contentY").toDouble();
    };
    const double before = screenTop("c0");

    // The reader is on the group row when an older page lands that ends with
    // the same turn's activity: the group gains a new leader, and the row the
    // reader was anchored to is no longer laid out. The group row, now shown
    // by the new leader, stays where the reader saw it.
    transcript->setProperty(
        "readingAnchor",
        QVariantMap{{"key", "c0"}, {"offset", 0}, {"followTail", false}, {"index", 0}});
    model->prepend({textRow("u", "user", "An older request with enough text to take up space."),
                    readRow("cp", "earlier step")});
    QCOMPARE(model->indexOfKey("c0"), -1);
    QVERIFY(geometry->rowForKey("cp") > 0);
    QVERIFY2(qAbs(screenTop("cp") - before) <= 1.0,
             qPrintable(QStringLiteral("group row moved %1 px").arg(screenTop("cp") - before)));
    QCOMPARE(transcript->property("readingAnchor").toMap().value("key").toString() != "c0", true);
}

void AgentWorkspaceViewTest::activityGroupToggleKeepsReadingAnchor() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QVariantList rows{textRow("u", "user", "Build it")};
    for (int index = 0; index < 8; ++index)
        rows.append(
            readRow(QStringLiteral("c%1").arg(index), QStringLiteral("step %1").arg(index)));
    for (int index = 0; index < 40; ++index)
        rows.append(textRow(QStringLiteral("a%1").arg(index), "assistant",
                            QStringLiteral("Paragraph %1 of the answer.").arg(index)));
    view.resetMessages(rows, "workspace:groups");
    QTRY_COMPARE(transcript->opacity(), 1.0);

    // Collapsed by default: the group is one row, and its members are not rows at all.
    QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtBeginning"));
    auto* model = transcript->property("messageModel").value<ketplus::AgentMessageModel*>();
    QVERIFY(model);
    QTRY_COMPARE(model->hiddenRowCount(), 7);
    QCOMPARE(model->renderedCount() - model->count(), 7);

    // Read below the group, then expand and collapse it above the reader.
    const int readingIndex = 20;
    readFrom(transcript, readingIndex, -40);
    QVERIFY(!transcript->property("followTail").toBool());
    QTRY_VERIFY(delegateAt(transcript, readingIndex));
    flushDeferredCalls();
    const QString readingKey = model->get(readingIndex).value("messageKey").toString();
    auto* reading = delegateAt(transcript, readingIndex);
    QVERIFY(reading);
    const auto screenY = [&] { return reading->mapToItem(transcript, QPointF()).y(); };
    const double before = screenY();

    for (const bool expanded : {true, false, true}) {
        QVERIFY(QMetaObject::invokeMethod(transcript, "setGroupExpanded", Q_ARG(QVariant, "c0"),
                                          Q_ARG(QVariant, expanded)));
        QTRY_COMPARE(model->hiddenRowCount(), expanded ? 0 : 7);
        flushDeferredCalls();
        reading = delegateForKey(transcript, readingKey);
        QVERIFY(reading);
        QVERIFY2(qAbs(screenY() - before) <= 2.0,
                 qPrintable(QStringLiteral("reader moved %1 px").arg(screenY() - before)));
    }
}

void AgentWorkspaceViewTest::groupExpansionSurvivesPagingAndIsPruned() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QVariantList rows{readRow("c2", "b"), readRow("c3", "c"), readRow("c4", "d")};
    for (int index = 0; index < 30; ++index)
        rows.append(textRow(QStringLiteral("f%1").arg(index), "assistant",
                            QStringLiteral("Filler paragraph %1.").arg(index)));
    view.resetMessages(rows, "workspace:paging-groups");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    auto* model = view.messageModel();
    QVERIFY(QMetaObject::invokeMethod(transcript, "setGroupExpanded", Q_ARG(QVariant, "c2"),
                                      Q_ARG(QVariant, true)));
    QVERIFY(groupExpanded(transcript, "c2"));

    // An older page changes the group's leader; the user's expansion stays.
    view.prependMessages({textRow("u", "user", "Go"), readRow("c1", "a")});
    QTRY_COMPARE(model->count(), 35);
    QCOMPARE(model->get(1).value("groupRole").toString(), QStringLiteral("leader"));
    flushDeferredCalls();
    QVERIFY(groupExpanded(transcript, model->get(2).value("groupKey").toString()));
    QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtBeginning"));
    QTRY_VERIFY(delegateAt(transcript, 3) && delegateAt(transcript, 3)->isVisible());

    // A newer page trims the leader from the front: still expanded, still remembered.
    QVariantList trimmed = rows.mid(1);
    trimmed.append(textRow("n1", "assistant", "Newer"));
    view.replaceMessageWindow(trimmed);
    QTRY_COMPARE(model->get(0).value("messageKey").toString(), QStringLiteral("c3"));
    flushDeferredCalls();
    QVERIFY(hasExplicitExpansion(transcript, "c2"));
    QVERIFY(groupExpanded(transcript, "c2"));

    // Trimming the whole group prunes its remembered state.
    view.replaceMessageWindow(trimmed.mid(2));
    QTRY_COMPARE(model->get(0).value("messageKey").toString(), QStringLiteral("f0"));
    QTRY_VERIFY(!hasExplicitExpansion(transcript, "c2"));
}

void AgentWorkspaceViewTest::streamedActivityJoinsGroupWithoutReset() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    view.setSessionState("Streaming", "Working", true);
    view.resetMessages({textRow("u", "user", "Go"), readRow("c1", "ls")}, "workspace:stream");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    auto* model = view.messageModel();
    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    view.appendMessage(readRow("c2", "cmake --build", true));
    view.appendMessage(readRow("c3", "ctest", true));
    QTRY_COMPARE(model->renderedCount(), 4);
    QCOMPARE(resets.size(), 0);
    QCOMPARE(model->get(1).value("groupSize").toInt(), 3);
    // A group of the running turn stays one summary line: its members are withheld...
    QTRY_COMPARE(model->hiddenRowCount(), 2);
    QCOMPARE(model->count(), 2);
    view.updateMessage(2, readRow("c2", "cmake --build"));
    view.updateMessage(3, readRow("c3", "ctest"));
    QTRY_VERIFY(!model->get(1).value("groupRunning").toBool());
    flushDeferredCalls();
    QVERIFY(!groupExpanded(transcript, "c1"));
    // ...and completing the turn changes nothing.
    view.setSessionState("Streaming", "Done", false);
    flushDeferredCalls();
    QCOMPARE(model->hiddenRowCount(), 2);
    QCOMPARE(model->count(), 2);
    QCOMPARE(resets.size(), 0);
}

void AgentWorkspaceViewTest::expandedGroupHangsUnderRail() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    view.resetMessages({textRow("u", "user", "Go"), readRow("c1", "ls"), readRow("c2", "pwd"),
                        readRow("c3", "git status")},
                       "workspace:rail");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    // Collapsed: one summary line whose chevron follows the text, and no rail.
    QQuickItem* summary = nullptr;
    QTRY_VERIFY((summary = findVisualItem(root, "transcriptGroupSummary")) != nullptr);
    QCOMPARE(summary->property("text").toString(), QStringLiteral("Read 3 files"));
    auto* chevron = findVisualItem(root, "transcriptGroupChevron");
    QVERIFY(chevron);
    QVERIFY(chevron->x() > summary->x() + summary->width() - 0.5);
    QVERIFY(chevron->x() < summary->x() + summary->width() + 8);
    QCOMPARE(chevron->property("name").toString(), QStringLiteral("chevron-down"));
    QVERIFY(delegateAt(transcript, 1));
    auto* collapsedRail = findVisualItem(delegateAt(transcript, 1), "transcriptGroupRail");
    QVERIFY(collapsedRail && !collapsedRail->isVisible());

    // Expanded: every member sits right of the rail with its kind's icon, as in the mock.
    QVERIFY(QMetaObject::invokeMethod(transcript, "setGroupExpanded", Q_ARG(QVariant, "c1"),
                                      Q_ARG(QVariant, true)));
    QTRY_COMPARE(view.messageModel()->count(), 4);
    QTRY_VERIFY((chevron = findVisualItem(root, "transcriptGroupChevron")) &&
                chevron->property("name").toString() == QStringLiteral("chevron-up"));
    for (int index = 1; index <= 3; ++index) {
        QQuickItem* row = nullptr;
        QTRY_VERIFY((row = delegateAt(transcript, index)) != nullptr);
        QQuickItem* rail = nullptr;
        QTRY_VERIFY((rail = findVisualItem(row, "transcriptGroupRail")) && rail->isVisible());
        auto* title = findVisualItem(row, "transcriptActivityTitle");
        QVERIFY(title);
        const qreal titleX = title->mapToItem(row, QPointF()).x();
        QVERIFY2(titleX > rail->x() + rail->width() + 8,
                 qPrintable(QStringLiteral("title at %1, rail at %2").arg(titleX).arg(rail->x())));
        auto* header = findVisualItem(row, "transcriptActivityHeader");
        QVERIFY(header);
        QVERIFY(title->mapToItem(header, QPointF()).x() > 0.0);
    }
}

// The mock's step line (TranscriptActivityItem.qml): a short title, then what the step acted
// on; a failure under it; a quiet group summary.
void AgentWorkspaceViewTest::activityStepsReadAsTheMock() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QVariantMap read = readRow("r", "Read: modules/ai/src/agent/qml/AgentSidebar.qml");
    read["command"] = QString();
    QVariantMap measure = readRow("m", "top -l 4");
    measure["subtype"] = "command";
    measure["commandDescription"] = "Measure idle CPU";
    QVariantMap tests = readRow("t", "ctest -R sidebar");
    tests["subtype"] = "command";
    tests["status"] = "failed";
    tests["exitCode"] = 8;
    tests["durationMs"] = 12400;
    // A failure leaves the summary's colour as it is without one.
    QVariantMap passed = tests;
    passed["status"] = "completed";
    view.resetMessages({textRow("u", "user", "Go"), read, measure, passed}, "workspace:quiet");
    QQuickItem* summary = nullptr;
    QTRY_VERIFY((summary = findVisualItem(root, "transcriptGroupSummary")) != nullptr);
    const QColor quiet = summary->property("color").value<QColor>();
    view.resetMessages({textRow("u", "user", "Go"), read, measure, tests}, "workspace:steps");
    QTRY_VERIFY((summary = findVisualItem(root, "transcriptGroupSummary")) != nullptr &&
                summary->property("text").toString().contains("failed"));
    QCOMPARE(summary->property("color").value<QColor>(), quiet);

    QVERIFY(QMetaObject::invokeMethod(transcript, "setGroupExpanded", Q_ARG(QVariant, "r"),
                                      Q_ARG(QVariant, true)));
    QTRY_COMPARE(view.messageModel()->count(), 4);
    const auto line = [&](int index) {
        QQuickItem* row = delegateAt(transcript, index);
        const auto text = [&](const char* name) {
            auto* item = row ? findVisualItem(row, name) : nullptr;
            return item && item->isVisible() ? item->property("text").toString() : QString();
        };
        return QStringList{text("transcriptActivityTitle"), text("transcriptActivityDetail"),
                           text("transcriptActivityStatus")};
    };
    QTRY_COMPARE(line(1), (QStringList{"Read", "modules/ai/src/agent/qml/AgentSidebar.qml", ""}));
    QTRY_COMPARE(line(2), (QStringList{"Measure idle CPU", "top -l 4", ""}));
    QTRY_COMPARE(line(3),
                 (QStringList{"Ran", "ctest -R sidebar", "Failed · exit code 8 · 12.4 s"}));
    // Seconds round before minutes split, so 119.6 s never reads "1m 60s".
    tests["durationMs"] = 119600;
    view.resetMessages({textRow("u", "user", "Go"), read, measure, tests}, "workspace:minutes");
    QTRY_VERIFY(findVisualItem(root, "transcriptGroupSummary") != nullptr);
    QVERIFY(QMetaObject::invokeMethod(transcript, "setGroupExpanded", Q_ARG(QVariant, "r"),
                                      Q_ARG(QVariant, true)));
    QTRY_COMPARE(view.messageModel()->count(), 4);
    QTRY_COMPARE(line(3).last(), QStringLiteral("Failed · exit code 8 · 2m 00s"));
}

void AgentWorkspaceViewTest::activityGroupCopyIncludesMembers() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    view.resetMessages({textRow("u", "user", "Go"), readRow("c1", "first-command"),
                        readRow("c2", "second-command"), readRow("c3", "third-command")});
    const auto copyText = [&] {
        QVariant text;
        QMetaObject::invokeMethod(root, "conversationCopyText", Q_RETURN_ARG(QVariant, text));
        return text.toString();
    };
    QTRY_VERIFY(copyText().contains(QStringLiteral("third-command")));
    for (const auto* command : {"first-command", "second-command", "third-command"})
        QVERIFY2(copyText().contains(QLatin1String(command)), qPrintable(copyText()));
}

void AgentWorkspaceViewTest::copiesMessageSourcesAndCodeBlocks() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    const QString answer =
        QStringLiteral("Use **this**:\n\n```cpp\nint value = 1;\nreturn value;\n```\n\nDone.");
    view.resetMessages({textRow("u", "user", "Show *code*"), textRow("a", "assistant", answer),
                        readRow("c", "cmake --build build"),
                        textRow("z", "assistant", QStringLiteral("Built it."))},
                       "workspace:copy");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtBeginning"));
    QTRY_VERIFY(delegateAt(transcript, 3));

    // Copy chrome is created lazily: nothing until a row is hovered or keyboard-focused,
    // except the turn's answer, which always offers Copy answer under it, as in the mock.
    QVERIFY(!childNamed(delegateAt(transcript, 0), "copyMessageButton"));
    QTRY_VERIFY(childNamed(delegateAt(transcript, 3), "copyMessageButton"));
    QVERIFY(!childNamed(delegateAt(transcript, 1), "copyCodeBlockButton"));
    QVERIFY(!childNamed(delegateAt(transcript, 2), "copyActivityButton"));
    view.quickView()->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(view.quickView()));
    transcript->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(transcript->hasActiveFocus());

    transcript->setProperty("keyboardIndex", 0);
    QTRY_VERIFY(childNamed(delegateAt(transcript, 0), "copyMessageButton"));
    auto* messageButton = childNamed(delegateAt(transcript, 0), "copyMessageButton");
    QVERIFY(!messageButton->property("copied").toBool());
    QVERIFY(clickButton(messageButton));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Show *code*"));
    // A click shows a check and "Copied", then the button returns to its idle state.
    QVERIFY(messageButton->property("copied").toBool());
    auto* copiedTip = messageButton->findChild<QObject*>("ketToolTip");
    QVERIFY(copiedTip);
    QCOMPARE(copiedTip->property("text").toString(), QStringLiteral("Copied"));
    QTRY_VERIFY_WITH_TIMEOUT(!messageButton->property("copied").toBool(), 3000);
    QCOMPARE(copiedTip->property("text").toString(), QStringLiteral("Copy message"));

    // Only the turn's last answer offers copy, and it copies what the agent wrote in the
    // turn, not the commands or files between.
    transcript->setProperty("keyboardIndex", 1);
    QTRY_VERIFY(!childNamed(delegateAt(transcript, 0), "copyMessageButton"));
    QTRY_VERIFY(childNamed(delegateAt(transcript, 1), "copyCodeBlockButton"));
    QVERIFY(!childNamed(delegateAt(transcript, 1), "copyMessageButton"));
    QVERIFY(clickButton(childNamed(delegateAt(transcript, 1), "copyCodeBlockButton")));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("int value = 1;\nreturn value;"));
    QVERIFY(
        childNamed(delegateAt(transcript, 1), "copyCodeBlockButton")->property("copied").toBool());

    transcript->setProperty("keyboardIndex", 2);
    QTest::qWait(50);
    QVERIFY(!childNamed(delegateAt(transcript, 2), "copyActivityButton"));
    QVERIFY(!childNamed(delegateAt(transcript, 2), "copyMessageButton"));

    transcript->setProperty("keyboardIndex", 3);
    QTRY_VERIFY(childNamed(delegateAt(transcript, 3), "copyMessageButton"));
    auto* answerButton = childNamed(delegateAt(transcript, 3), "copyMessageButton");
    QCOMPARE(answerButton->findChild<QObject*>("ketToolTip")->property("text").toString(),
             QStringLiteral("Copy answer"));
    if (const auto capture = qEnvironmentVariable("KETPLUS_ANSWER_COPY_CAPTURE");
        !capture.isEmpty()) {
        QTest::qWait(150);
        QVERIFY(view.quickView()->grabWindow().save(capture));
    }
    QVERIFY(clickButton(answerButton));
    // The Markdown source, not the rendered text.
    QCOMPARE(QGuiApplication::clipboard()->text(), answer + QStringLiteral("\n\nBuilt it."));

    // Unlike the rest of the row chrome, it stays when the hover or focus leaves.
    transcript->setProperty("keyboardIndex", 0);
    QTest::qWait(50);
    QVERIFY(childNamed(delegateAt(transcript, 3), "copyMessageButton"));
}

void AgentWorkspaceViewTest::listCodeBlocksStayInTheirItem() {
    using ketplus::agent_message::qtMarkdownSource;
    // A reply giving a command per step, as a tight list.
    const QString steps = QStringLiteral("Run:\n\n1. Restart the daemon:\n   ```\n   pkill daemon\n"
                                         "   ```\n2. Open the app:\n   ```sh\n   open App.app\n"
                                         "   ```\n3. Install.\n\nDone.");
    QTextDocument document;
    document.setMarkdown(qtMarkdownSource(steps));
    struct Row {
        QString text;
        int number; // 0 outside the list
        bool code;
    };
    QList<Row> rows;
    for (auto block = document.begin(); block.isValid(); block = block.next())
        rows.append({block.text(), block.textList() ? block.textList()->itemNumber(block) + 1 : 0,
                     block.blockFormat().hasProperty(QTextFormat::BlockCodeFence)});
    QCOMPARE(rows.size(), 7);
    const QList<std::tuple<QString, int, bool>> expected{
        {"Run:", 0, false},          {"Restart the daemon:", 1, false}, {"pkill daemon", 0, true},
        {"Open the app:", 2, false}, {"open App.app", 0, true},         {"Install.", 3, false},
        {"Done.", 0, false}};
    for (qsizetype index = 0; index < rows.size(); ++index) {
        QCOMPARE(rows.at(index).text, std::get<0>(expected.at(index)));
        QCOMPARE(rows.at(index).number, std::get<1>(expected.at(index)));
        QCOMPARE(rows.at(index).code, std::get<2>(expected.at(index)));
    }
    // Only fences inside list items change; code copied from the source is untouched.
    const QString plain = QStringLiteral("Text\n```\n1. not a list\n```\nMore");
    QCOMPARE(qtMarkdownSource(plain), plain);
    QCOMPARE(ketplus::agent_message::fencedCodeBlocks(qtMarkdownSource(steps)),
             ketplus::agent_message::fencedCodeBlocks(steps));
}

void AgentWorkspaceViewTest::fencedCodeBlockSources() {
    using ketplus::agent_message::fencedCodeBlocks;
    QCOMPARE(fencedCodeBlocks(QStringLiteral("a\n```\none\n```\nb\n~~~~py\n```\ntwo\n~~~~\n")),
             QStringList({QStringLiteral("one"), QStringLiteral("```\ntwo")}));
    QCOMPARE(fencedCodeBlocks(QStringLiteral("```sh\nopen")),
             QStringList({QStringLiteral("open")}));
    QCOMPARE(fencedCodeBlocks(QStringLiteral("no code `inline`")), QStringList());
    // Chunked assistant messages copy the complete source, not one block.
    ketplus::AgentMessageModel model;
    const QString giant = QStringLiteral("Paragraph text.\n\n").repeated(4000);
    model.reset({textRow("giant", "assistant", giant)});
    QVERIFY(model.count() > 1);
    QCOMPARE(model.sourceForRow(model.count() - 1).value("body").toString(), giant);
}

void AgentWorkspaceViewTest::conversationCopyWhileRunning() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    view.setSessionState("Copy", "Working", true);
    view.resetMessages(
        {textRow("u", "user", "Loaded question"), textRow("a", "assistant", "Streaming answer")});
    auto* menu = root->findChild<QObject*>(QStringLiteral("transcriptContextMenu"));
    QVERIFY(menu);
    auto* copyItem =
        menu->findChild<QObject*>(QStringLiteral("copyTranscriptConversationMenuItem"));
    QVERIFY(copyItem);
    QCOMPARE(copyItem->property("text").toString(), QStringLiteral("Copy loaded conversation"));
    QGuiApplication::clipboard()->clear();
    QVERIFY(QMetaObject::invokeMethod(copyItem, "triggered"));
    QTRY_VERIFY(QGuiApplication::clipboard()->text().contains(QStringLiteral("Streaming answer")));
    QVERIFY(QGuiApplication::clipboard()->text().contains(QStringLiteral("Loaded question")));
}

void AgentWorkspaceViewTest::findCountsNavigatesAndExpandsGroups() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QVariantList rows{textRow("u", "user", "Find Alpha please"),
                      textRow("a", "assistant", "alpha is here.\n\nAnd **alpha** again."),
                      readRow("c1", "ls"), readRow("c2", "echo alpha"),
                      textRow("z", "assistant", "No match here")};
    for (int index = 0; index < 30; ++index)
        rows.append(textRow(QStringLiteral("f%1").arg(index), "assistant",
                            QStringLiteral("Filler paragraph %1.").arg(index)));
    view.resetMessages(rows, "workspace:find");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtBeginning"));
    QTRY_VERIFY(delegateAt(transcript, 0));

    view.openTranscriptFind();
    auto* bar = root->findChild<QQuickItem*>("transcriptFindBar");
    QVERIFY(bar);
    QTRY_VERIFY(bar->isVisible());
    call(transcript, "setFindQuery", "alpha");
    QCOMPARE(transcript->property("findResults").toList().size(), 4);
    QCOMPARE(transcript->property("findCountText").toString(), QStringLiteral("1 / 4"));
    QCOMPARE(transcript->property("findCurrentKey").toString(), QStringLiteral("u"));

    QStringList order;
    for (int step = 0; step < 4; ++step) {
        view.findInTranscript(false);
        order.append(transcript->property("findCurrentKey").toString());
    }
    QCOMPARE(order, QStringList({"a", "a", "c2", "u"}));
    QCOMPARE(transcript->property("findCountText").toString(), QStringLiteral("1 / 4"));
    // Previous from the first match with no older history wraps to the last match.
    view.findInTranscript(true);
    QCOMPARE(transcript->property("findCurrentKey").toString(), QStringLiteral("c2"));
    // The collapsed group was expanded so its member is visible and scrolled to.
    QVERIFY(groupExpanded(transcript, "c1"));
    QTRY_VERIFY(delegateAt(transcript, 3) && delegateAt(transcript, 3)->isVisible());
    QVERIFY(!transcript->property("followTail").toBool());
    QTRY_VERIFY(childNamed(delegateAt(transcript, 3), "findActivityHighlight") &&
                childNamed(delegateAt(transcript, 3), "findActivityHighlight")->isVisible());

    // The current assistant match is highlighted in its rendered text.
    view.findInTranscript(false);
    view.findInTranscript(false);
    QTRY_VERIFY(delegateAt(transcript, 1));
    QTRY_VERIFY(childNamed(delegateAt(transcript, 1), "findHighlight") &&
                childNamed(delegateAt(transcript, 1), "findHighlight")->isVisible());

    call(transcript, "setFindQuery", "missing");
    QCOMPARE(transcript->property("findCountText").toString(), QStringLiteral("No results"));
    auto* field = root->findChild<QQuickItem*>("transcriptFindField");
    QVERIFY(field);
    pressKey(field, Qt::Key_Escape);
    QTRY_VERIFY(!bar->isVisible());
}

void AgentWorkspaceViewTest::findSearchesOlderHistoryWithinBound() {
    for (const int matchPage : {3, -1}) {
        ketplus::AgentWorkspaceView view;
        view.applyTheme(testPalette());
        view.resize(900, 640);
        view.show();
        auto* root = view.quickView()->rootObject();
        auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        int oldest = 1000;
        QVariantList initial;
        for (int index = oldest; index < oldest + 6; ++index)
            initial.append(textRow(QStringLiteral("m%1").arg(index), "assistant",
                                   QStringLiteral("Message %1").arg(index),
                                   QStringLiteral("turn-%1").arg(index)));
        view.resetMessages(initial, QStringLiteral("workspace:older-%1").arg(matchPage));
        QTRY_COMPARE(transcript->opacity(), 1.0);
        transcript->setProperty("idlePrefetchDone", true);
        view.setHistoryState({{"hasOlder", true}});
        flushDeferredCalls();

        int served = 0;
        int pagesServed = 0;
        QObject::connect(
            &view, &ketplus::AgentWorkspaceView::olderMessagesRequested, &view,
            [&](double, double, qint64 token) {
                QTimer::singleShot(0, &view, [&, token] {
                    // Count pages loaded by the find search; once a match is
                    // shown, ordinary distance prefetch may continue paging.
                    if (transcript->property("findSearchingOlder").toBool())
                        ++served;
                    const int pageNumber = ++pagesServed;
                    QVariantList page;
                    for (int row = 0; row < 2; ++row) {
                        --oldest;
                        const bool needle = pageNumber == matchPage && row == 1;
                        page.prepend(textRow(QStringLiteral("m%1").arg(oldest), "assistant",
                                             needle ? QStringLiteral("the needle is here")
                                                    : QStringLiteral("Older %1").arg(oldest),
                                             QStringLiteral("turn-%1").arg(oldest)));
                    }
                    view.prependMessages(page);
                    view.setHistoryState({{"hasOlder", true}, {"requestToken", token}});
                });
            });

        view.openTranscriptFind();
        call(transcript, "setFindQuery", "needle");
        QCOMPARE(transcript->property("findCountText").toString(), QStringLiteral("No results"));
        view.findInTranscript(true);
        QVERIFY(transcript->property("findSearchingOlder").toBool());
        QCOMPARE(root->findChild<QQuickItem*>("transcriptFindStatus")->property("text").toString(),
                 QStringLiteral("Searching older history…"));
        QTRY_VERIFY_WITH_TIMEOUT(!transcript->property("findSearchingOlder").toBool(), 10000);
        if (matchPage > 0) {
            QCOMPARE(served, matchPage);
            QCOMPARE(transcript->property("findCountText").toString(), QStringLiteral("1 / 1"));
            const QString key = transcript->property("findCurrentKey").toString();
            QVERIFY(view.messageModel()
                        ->get(view.messageModel()->indexOfKey(key))
                        .value("body")
                        .toString()
                        .contains(QStringLiteral("needle")));
            QVERIFY(transcript->property("findStatus").toString().isEmpty());
        } else {
            QCOMPARE(served, transcript->property("findOlderPageLimit").toInt());
            QVERIFY2(transcript->property("findStatus")
                         .toString()
                         .contains(QStringLiteral("10 older pages")),
                     qPrintable(transcript->property("findStatus").toString()));
            // The next Previous continues with another bounded burst.
            view.findInTranscript(true);
            QVERIFY(transcript->property("findSearchingOlder").toBool());
            QTRY_VERIFY_WITH_TIMEOUT(!transcript->property("findSearchingOlder").toBool(), 10000);
            QCOMPARE(served, 20);
        }
    }
}

void AgentWorkspaceViewTest::findIgnoresUnrelatedHistoryStateUpdates() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QVariantList initial;
    for (int index = 0; index < 6; ++index)
        initial.append(textRow(QStringLiteral("m%1").arg(index), "assistant",
                               QStringLiteral("Message %1").arg(index)));
    view.resetMessages(initial, "workspace:find-spurious");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    transcript->setProperty("idlePrefetchDone", true);
    // History is busy: the find cannot request a page yet and waits on its retry timer.
    view.setHistoryState({{"hasOlder", true}, {"loading", true}});

    int requests = 0;
    QObject::connect(&view, &ketplus::AgentWorkspaceView::olderMessagesRequested, &view,
                     [&](double, double, qint64 token) {
                         // Pages requested by the find (ordinary prefetch may follow the match).
                         if (transcript->property("findSearchingOlder").toBool())
                             ++requests;
                         // An unrelated update while the page is in flight.
                         QTimer::singleShot(0, &view,
                                            [&] { view.setHistoryState({{"hasOlder", true}}); });
                         QTimer::singleShot(30, &view, [&, token] {
                             view.prependMessages({textRow("p0", "assistant", "Older"),
                                                   textRow("p1", "assistant", "the needle")});
                             view.setHistoryState({{"hasOlder", true}, {"requestToken", token}});
                         });
                     });

    view.openTranscriptFind();
    call(transcript, "setFindQuery", "needle");
    view.findInTranscript(true);
    QVERIFY(transcript->property("findSearchingOlder").toBool());
    QCOMPARE(requests, 0);
    // An unrelated update (no page of the find's completed, older edge unchanged) must not
    // count as a searched page or end the search.
    view.setHistoryState({{"hasOlder", true}});
    flushDeferredCalls();
    QVERIFY(transcript->property("findSearchingOlder").toBool());
    QCOMPARE(transcript->property("findOlderPagesSearched").toInt(), 0);

    QTRY_VERIFY_WITH_TIMEOUT(!transcript->property("findSearchingOlder").toBool(), 10000);
    QCOMPARE(requests, 1);
    QCOMPARE(transcript->property("findOlderPagesSearched").toInt(), 1);
    QCOMPARE(transcript->property("findCurrentKey").toString(), QStringLiteral("p1"));
    QCOMPARE(transcript->property("findCountText").toString(), QStringLiteral("1 / 1"));
}

void AgentWorkspaceViewTest::findGoesToLastResultWhenAnchorTrimmed() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QVariantList initial;
    for (int index = 1000; index < 1006; ++index)
        initial.append(textRow(QStringLiteral("m%1").arg(index), "assistant",
                               index == 1005 ? QStringLiteral("the newest needle")
                                             : QStringLiteral("Message %1").arg(index)));
    view.resetMessages(initial, "workspace:find-trim");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    transcript->setProperty("idlePrefetchDone", true);
    view.setHistoryState({{"hasOlder", true}});
    flushDeferredCalls();

    int requests = 0;
    int served = 0;
    QObject::connect(
        &view, &ketplus::AgentWorkspaceView::olderMessagesRequested, &view,
        [&](double, double, qint64 token) {
            // Pages requested by the find (ordinary prefetch may follow the match).
            if (transcript->property("findSearchingOlder").toBool())
                ++requests;
            const int request = ++served;
            QTimer::singleShot(0, &view, [&, token, request] {
                if (request == 1) {
                    // The older page arrives with a window trim that drops the anchor match.
                    QVariantList window{textRow("m998", "assistant", "Older"),
                                        textRow("m999", "assistant", "an older needle")};
                    window.append(initial.mid(0, 5));
                    view.replaceMessageWindow(window);
                } else {
                    view.prependMessages(
                        {textRow(QStringLiteral("o%1").arg(request), "assistant", "Older")});
                }
                view.setHistoryState({{"hasOlder", true}, {"requestToken", token}});
            });
        });

    view.openTranscriptFind();
    call(transcript, "setFindQuery", "needle");
    QCOMPARE(transcript->property("findCurrentKey").toString(), QStringLiteral("m1005"));
    view.findInTranscript(true);
    QVERIFY(transcript->property("findSearchingOlder").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!transcript->property("findSearchingOlder").toBool(), 10000);
    QCOMPARE(requests, 1);
    QCOMPARE(view.messageModel()->indexOfKey(QStringLiteral("m1005")), -1);
    QCOMPARE(transcript->property("findCurrentKey").toString(), QStringLiteral("m999"));
    QCOMPARE(transcript->property("findCountText").toString(), QStringLiteral("1 / 1"));
}

void AgentWorkspaceViewTest::findResultsRefreshOnModelChanges() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QVariantList rows{textRow("u", "user", "alpha question"),
                      textRow("a", "assistant", "plain answer")};
    for (int index = 0; index < 10; ++index)
        rows.append(textRow(QStringLiteral("f%1").arg(index), "assistant",
                            QStringLiteral("Filler %1").arg(index)));
    view.resetMessages(rows, "workspace:find-live");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    view.openTranscriptFind();

    // A source row that changes (streamed text) is searched again.
    call(transcript, "setFindQuery", "beta");
    QCOMPARE(transcript->property("findCountText").toString(), QStringLiteral("No results"));
    view.updateMessage(1, textRow("a", "assistant", "now beta appears"));
    QTRY_COMPARE(transcript->property("findResults").toList().size(), 1);

    // The current match disappearing clears the current highlight.
    call(transcript, "setFindQuery", "alpha");
    QCOMPARE(transcript->property("findCurrentKey").toString(), QStringLiteral("u"));
    view.updateMessage(0, textRow("u", "user", "a question"));
    QTRY_COMPARE(transcript->property("findCountText").toString(), QStringLiteral("No results"));
    QCOMPARE(transcript->property("findCurrentKey").toString(), QString());

    // Streamed deltas while a turn runs keep results current (coalesced refreshes).
    view.setSessionState("Streaming", "Working", true);
    call(transcript, "setFindQuery", "gamma");
    QString streamed = QStringLiteral("stream");
    view.appendMessage(textRow("s", "assistant", streamed));
    for (int delta = 0; delta < 20; ++delta) {
        streamed += delta == 10 ? QStringLiteral(" gamma") : QStringLiteral(" more");
        view.updateMessage(view.messageModel()->count() - 1, textRow("s", "assistant", streamed));
    }
    QTRY_COMPARE(transcript->property("findResults").toList().size(), 1);
    view.setSessionState("Streaming", "Done", false);

    // A model reset refreshes too.
    call(transcript, "setFindQuery", "beta");
    QCOMPARE(transcript->property("findResults").toList().size(), 1);
    view.resetMessages({textRow("x", "assistant", "nothing here")}, "workspace:find-live");
    QTRY_COMPARE(transcript->property("findCountText").toString(), QStringLiteral("No results"));
}

void AgentWorkspaceViewTest::messagesExposeAccessibleSemantics() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    view.resetMessages({textRow("u", "user", "Please   fix\nthe build"),
                        textRow("a", "assistant", "Fixed it."), readRow("c1", "ls"),
                        [] {
                            auto row = readRow("c2", "make");
                            row.insert("status", "failed");
                            return row;
                        }()},
                       "workspace:a11y");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtBeginning"));
    QTRY_VERIFY(delegateAt(transcript, 2));
    const auto accessible = [&](int index, const char* property) {
        QVariant info;
        QMetaObject::invokeMethod(transcript, "accessibleInfo", Q_RETURN_ARG(QVariant, info),
                                  Q_ARG(QVariant, index));
        return info.toMap().value(QLatin1String(property));
    };
    QCOMPARE(accessible(0, "role").toInt(), int(QAccessible::ListItem));
    QCOMPARE(accessible(0, "name").toString(), QStringLiteral("You: Please fix the build"));
    QCOMPARE(accessible(1, "name").toString(), QStringLiteral("Codex: Fixed it."));
    QCOMPARE(accessible(2, "name").toString(), QStringLiteral("Read 2 files (1 failed)"));
    QCOMPARE(accessible(2, "description").toString(),
             QStringLiteral("collapsed, completed, 1 failed"));
    QCOMPARE(accessible(-1, "role").toInt(), int(QAccessible::List));
}

void AgentWorkspaceViewTest::keyboardNavigatesMessagesAndGroups() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    view.resetMessages({textRow("u", "user", "Question"), readRow("c1", "ls"), readRow("c2", "pwd"),
                        textRow("a", "assistant", "Answer body")},
                       "workspace:keys");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtBeginning"));
    view.quickView()->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(view.quickView()));
    transcript->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(transcript->hasActiveFocus());
    pressKey(transcript, Qt::Key_Down);
    QCOMPARE(transcript->property("keyboardIndex").toInt(), 0);
    pressKey(transcript, Qt::Key_Down);
    QCOMPARE(transcript->property("keyboardIndex").toInt(), 1);
    // The collapsed group is one row, so the next stop is the assistant answer.
    pressKey(transcript, Qt::Key_Down);
    QCOMPARE(transcript->property("keyboardIndex").toInt(), 2);
    pressKey(transcript, Qt::Key_Up);
    QCOMPARE(transcript->property("keyboardIndex").toInt(), 1);
    QTRY_VERIFY(childNamed(delegateAt(transcript, 1), "keyboardFocusRing") &&
                childNamed(delegateAt(transcript, 1), "keyboardFocusRing")->isVisible());
    pressKey(transcript, Qt::Key_Return);
    QVERIFY(groupExpanded(transcript, "c1"));
    // Expanding brings the member back as a row of its own.
    pressKey(transcript, Qt::Key_Down);
    QCOMPARE(transcript->property("keyboardIndex").toInt(), 2);
    pressKey(transcript, Qt::Key_Down);
    // StandardKey.Copy is Cmd+C on macOS (Qt maps Cmd to ControlModifier).
    pressKey(transcript, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Answer body"));
    // A click hides the keyboard focus ring.
    transcript->setProperty("keyboardFocusVisible", false);
    QTRY_VERIFY(!childNamed(delegateAt(transcript, 3), "keyboardFocusRing"));
}

void AgentWorkspaceViewTest::jumpToLatestCountsNewMessages() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVariantList rows;
    for (int index = 0; index < 40; ++index)
        rows.append(textRow(QStringLiteral("h%1").arg(index), "assistant",
                            QStringLiteral("History paragraph %1.").arg(index)));
    view.setSessionState("Streaming", "Working", true);
    view.resetMessages(rows, "workspace:badge");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QTRY_VERIFY(transcript->property("contentHeight").toDouble() > transcript->height());
    readFrom(transcript, 5);
    QVERIFY(!transcript->property("followTail").toBool());
    auto* badge = root->findChild<QQuickItem*>("newMessageBadge");
    QVERIFY(badge);
    QVERIFY(!badge->isVisible());

    view.appendMessage(textRow("u2", "user", "Another question", "turn-2"));
    view.appendMessage(readRow("n1", "ls", true, "turn-2"));
    view.appendMessage(readRow("n2", "pwd", true, "turn-2"));
    view.appendMessage(readRow("n3", "make", true, "turn-2"));
    auto answer = textRow("a2", "assistant", "Streaming", "turn-2");
    view.appendMessage(answer);
    for (const auto* delta : {" more", " and more"}) {
        answer["body"] = answer["body"].toString() + QLatin1String(delta);
        view.updateMessage(view.messageModel()->count() - 1, answer);
    }
    // user + one activity group + assistant, regardless of deltas and group members.
    QTRY_COMPARE(transcript->property("newMessageCount").toInt(), 3);
    QTRY_VERIFY(badge->isVisible());
    QCOMPARE(root->findChild<QQuickItem*>("newMessageBadgeLabel")->property("text").toString(),
             QStringLiteral("3 new"));
    QVERIFY(transcript->property("followTail").toBool() == false);

    auto* jump = root->findChild<QQuickItem*>("jumpToLatestButton");
    QVERIFY(clickButton(jump));
    QTRY_COMPARE(transcript->property("newMessageCount").toInt(), 0);
    QVERIFY(!badge->isVisible());
    // At the tail nothing is counted.
    view.appendMessage(textRow("a3", "assistant", "Live", "turn-3"));
    flushDeferredCalls();
    QCOMPARE(transcript->property("newMessageCount").toInt(), 0);
}

void AgentWorkspaceViewTest::newMessageBadgeIgnoresNewerPagesAndMergedActivity() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QVariantList rows;
    for (int index = 0; index < 40; ++index)
        rows.append(textRow(QStringLiteral("h%1").arg(index), "assistant",
                            QStringLiteral("History paragraph %1.").arg(index)));
    view.resetMessages(rows, "workspace:badge-pages");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    transcript->setProperty("idlePrefetchDone", true);
    QTRY_VERIFY(transcript->property("contentHeight").toDouble() > transcript->height());
    readFrom(transcript, 5);
    QVERIFY(!transcript->property("followTail").toBool());
    // A newer-history page whose state (hasNewer false) lands before its rows.
    QObject::connect(&view, &ketplus::AgentWorkspaceView::newerMessagesRequested, &view,
                     [&](double, double, qint64 token) {
                         QTimer::singleShot(0, &view, [&, token] {
                             view.setHistoryState({{"hasNewer", false}, {"requestToken", token}});
                             view.appendMessage(textRow("p1", "assistant", "Paged newer 1"));
                             view.appendMessage(textRow("p2", "assistant", "Paged newer 2"));
                         });
                     });
    // Connected first: the prefetch distance can ask for the page as soon as
    // newer history exists.
    view.setHistoryState({{"hasNewer", true}, {"canLoadNewer", true}});
    flushDeferredCalls();

    if (!transcript->property("historyRequestPending").toBool())
        call(transcript, "requestHistoryPage", "newer");
    QTRY_VERIFY(view.messageModel()->indexOfKey(QStringLiteral("p2")) >= 0);
    QTRY_VERIFY(!transcript->property("historyRequestPending").toBool());
    flushDeferredCalls();
    QCOMPARE(transcript->property("newMessageCount").toInt(), 0);
    QVERIFY(!transcript->property("followTail").toBool());

    view.appendMessage(textRow("l1", "assistant", "Live", "turn-2"));
    QTRY_COMPARE(transcript->property("newMessageCount").toInt(), 1);

    // Two single activity rows counted separately merge into one group: one message.
    view.appendMessage(readRow("a1", "ls", false, "turn-3"));
    view.appendMessage(readRow("x", "mid", false, QString()));
    view.appendMessage(readRow("a2", "pwd", false, "turn-3"));
    QTRY_COMPARE(transcript->property("newMessageCount").toInt(), 4);
    view.updateMessage(view.messageModel()->indexOfKey(QStringLiteral("x")),
                       readRow("x", "mid", false, "turn-3"));
    QTRY_COMPARE(
        view.messageModel()->get(view.messageModel()->indexOfKey("a1")).value("groupSize").toInt(),
        3);
    QTRY_COMPARE(transcript->property("newMessageCount").toInt(), 2);
}

void AgentWorkspaceViewTest::collapsedGroupDelegateCost() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QVariantList rows{textRow("u", "user", "Build it")};
    for (int index = 0; index < 300; ++index)
        rows.append(
            readRow(QStringLiteral("c%1").arg(index), QStringLiteral("step %1").arg(index)));
    for (int index = 0; index < 20; ++index)
        rows.append(textRow(QStringLiteral("a%1").arg(index), "assistant",
                            QStringLiteral("Paragraph %1 of the answer.").arg(index)));
    view.resetMessages(rows, "workspace:cost");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtBeginning"));
    QTRY_VERIFY(delegateAt(transcript, 0));
    auto* contentItem = transcript->property("contentItem").value<QQuickItem*>();
    QVERIFY(contentItem);
    auto* model = transcript->property("messageModel").value<ketplus::AgentMessageModel*>();
    QVERIFY(model);
    int delegates = 0;
    int members = 0;
    const auto measure = [&] {
        delegates = members = 0;
        for (QQuickItem* child : contentItem->childItems()) {
            if (!child->property("messageKey").isValid())
                continue;
            ++delegates;
            if (child->property("groupRole").toString() == QStringLiteral("member"))
                ++members;
        }
        return delegates > 0;
    };
    QTRY_VERIFY(measure());
    qInfo("collapsed 300-command group: %d delegates, %d of them members, %d rows withheld",
          delegates, members, model->hiddenRowCount());
    // The whole point: a collapsed group costs one row and one delegate, so
    // ListView never lays out, measures or averages a zero-height row.
    QCOMPARE(members, 0);
    QCOMPARE(model->hiddenRowCount(), 299);

    // Keyboard navigation steps over the whole group at once.
    view.quickView()->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(view.quickView()));
    transcript->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(transcript->hasActiveFocus());
    transcript->setProperty("keyboardIndex", 1);
    pressKey(transcript, Qt::Key_Down);
    // One row for the whole 300-command group, so the next stop is the row after it.
    QCOMPARE(transcript->property("keyboardIndex").toInt(), 2);
    pressKey(transcript, Qt::Key_Up);
    QCOMPARE(transcript->property("keyboardIndex").toInt(), 1);
}

void AgentWorkspaceViewTest::longCommandTitleStaysInsideColumn() {
    // Middle elision only applies to single-line text: a multi-line command
    // drew its whole first line past the column.
    const QString command =
        QStringLiteral("export PATH=\"$HOME/bin:$PATH\"; cd ~/src/project && S=/tmp/") +
        QString(QStringLiteral("segment/")).repeated(40) +
        QStringLiteral(
            " && diff <(sort a.txt) <(sort b.txt) 2>/dev/null\nnode <<'JS'\nconsole.log(1)\nJS");
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(700, 480);
    view.show();
    view.resetMessages(
        {textRow("u", "user", "Run it"), readRow("c1", QStringLiteral("Ran: ") + command)},
        QStringLiteral("long-command"));
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QQuickItem* title = nullptr;
    QTRY_VERIFY((title = findVisualItem(root, "transcriptActivityTitle")) != nullptr);
    auto* header = findVisualItem(root, "transcriptActivityHeader");
    QVERIFY(header);
    const auto fits = [&] {
        const auto right =
            title->mapToItem(header, QPointF(title->property("contentWidth").toReal(), 0)).x();
        return right <= header->width() + 0.5;
    };
    QTRY_VERIFY(fits());
    QVERIFY(!title->property("text").toString().contains(QLatin1Char('\n')));
    auto* row = findVisualItem(root, "transcriptActivityRow");
    QVERIFY(row);
    QVERIFY(QMetaObject::invokeMethod(row, "toggleDetails"));
    QTRY_VERIFY(row->property("expanded").toBool());
    QTRY_VERIFY(fits());
}

void AgentWorkspaceViewTest::warmSwitchLaysOutTheActiveConversation() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QVariantList longRows;
    for (int index = 0; index < 120; ++index)
        longRows.append(textRow(QStringLiteral("long%1").arg(index), "assistant",
                                QStringLiteral("Paragraph %1 of a long conversation.").arg(index)));
    const QVariantList shortRows{textRow("s0", "user", "A short request"),
                                 textRow("s1", "assistant", "A short answer")};
    const auto transcript = [&] { return root->findChild<QQuickItem*>("agentTranscript"); };
    // The flickable's extent must be the active conversation's rows, not the
    // previous conversation's: a warm model publishes no geometry change of its
    // own, and a stale, taller extent parks the tail far below the rows.
    const auto laidOut = [&](const char* shownKey) {
        auto* view = transcript();
        auto* model = view->property("messageModel").value<ketplus::AgentMessageModel*>();
        const double rows = model->geometry()->totalHeight();
        const double extent = view->property("contentHeight").toDouble();
        return extent >= rows && extent < rows + 400 && delegateForKey(view, shownKey);
    };

    view.resetMessages(longRows, "workspace:long");
    QTRY_VERIFY(laidOut("long119"));
    view.resetMessages(shortRows, "workspace:short");
    QTRY_VERIFY(laidOut("s1"));
    view.resetMessages(longRows, "workspace:long");
    QTRY_VERIFY(laidOut("long119"));
    view.resetMessages(shortRows, "workspace:short");
    QTRY_VERIFY(laidOut("s1"));
}

void AgentWorkspaceViewTest::approvalCardStaysOutsideCommandGroup() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    // A permission request arrives between the commands of a running turn.
    QVariantMap approval{
        {"messageKey", "permission"},
        {"kind", "tool"},
        {"category", "activity"},
        {"subtype", "approval"},
        {"turnId", "turn-1"},
        {"body", "Allow the agent to run the build?"},
        {"command", "cmake --build build/dev"},
        {"running", true},
        {"questionAnswers", QVariantMap{}},
        {"questions",
         QVariantList{QVariantMap{
             {"id", "decision"},
             {"question", "Allow the agent to run the build?"},
             {"options", QVariantList{QVariantMap{{"id", "a"}, {"label", "Accept"}},
                                      QVariantMap{{"id", "d"}, {"label", "Decline"}}}}}}},
        {"filePath", ""}};
    view.resetMessages({textRow("u", "user", "Build it"), readRow("c1", "ls"),
                        readRow("c2", "git status"), approval, readRow("c3", "pwd")},
                       "workspace:approval");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    auto* model = transcript->property("messageModel").value<ketplus::AgentMessageModel*>();
    QVERIFY(model);
    // The request is a row of its own, never a collapsed group member.
    QVERIFY(model->indexOfKey("permission") >= 0);
    QCOMPARE(model->get(model->indexOfKey("permission")).value("groupRole").toString(), QString());
    QQuickItem* card = nullptr;
    QTRY_VERIFY((card = findVisualItem(root, QStringLiteral("messageApprovalCard"))));
    QTRY_VERIFY(card->isVisible() && card->height() > 0);
    auto* accept = findVisualItem(card, QStringLiteral("approvalAcceptButton"));
    QVERIFY(accept && accept->isVisible());
    // The status sits inside the card, right-aligned with its padding.
    auto* status = findVisualItem(card, QStringLiteral("approvalStatus"));
    QVERIFY(status);
    const QRectF bounds = status->mapRectToItem(card, status->boundingRect());
    QVERIFY2(bounds.right() <= card->width(),
             qPrintable(
                 QStringLiteral("status ends at %1 of %2").arg(bounds.right()).arg(card->width())));
}

void AgentWorkspaceViewTest::copiesMouseSelectionWithShortcut() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* window = view.quickView();
    auto* transcript = window->rootObject()->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    const QString answer = QStringLiteral("Alpha beta gamma delta epsilon zeta eta theta");
    view.resetMessages({textRow("u", "user", "Question"), textRow("a", "assistant", answer)},
                       "workspace:selection");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(window));
    QTRY_VERIFY(childNamed(delegateAt(transcript, 1), "messageBody"));
    auto* body = childNamed(delegateAt(transcript, 1), "messageBody");

    const QPointF from = body->mapToScene(QPointF(2, body->height() / 2));
    const QPointF to = body->mapToScene(QPointF(120, body->height() / 2));
    QTest::mousePress(window, Qt::LeftButton, {}, from.toPoint());
    for (int step = 1; step <= 10; ++step)
        QTest::mouseMove(window, (from + (to - from) * step / 10.0).toPoint(), 5);
    QTest::mouseRelease(window, Qt::LeftButton, {}, to.toPoint(), 5);
    const QString selected = body->property("selectedText").toString();
    QVERIFY2(selected.size() > 3 && answer.startsWith(selected), qPrintable(selected));
    QCOMPARE(window->activeFocusItem(), body);

    // The text claims Copy and Select All before an application menu shortcut can take them.
    for (const QKeySequence::StandardKey key : {QKeySequence::Copy, QKeySequence::SelectAll}) {
        const QKeyCombination combination = QKeySequence(key)[0];
        QKeyEvent shortcutOverride(QEvent::ShortcutOverride, combination.key(),
                                   combination.keyboardModifiers());
        shortcutOverride.ignore();
        QCoreApplication::sendEvent(body, &shortcutOverride);
        QVERIFY(shortcutOverride.isAccepted());
    }
    QGuiApplication::clipboard()->setText(QStringLiteral("before"));
    QTest::keySequence(window, QKeySequence::Copy);
    QCOMPARE(QGuiApplication::clipboard()->text(), selected);
    QTest::keySequence(window, QKeySequence::SelectAll);
    QCOMPARE(body->property("selectedText").toString(), answer);

    // A right-click offers the selection and the whole conversation.
    auto* menu = transcript->findChild<QObject*>("transcriptContextMenu");
    QVERIFY(menu);
    QTest::mouseClick(window, Qt::RightButton, {}, from.toPoint());
    QTRY_VERIFY(menu->property("visible").toBool());
    auto* copyItem = menu->findChild<QObject*>("copyTranscriptSelectionMenuItem");
    QVERIFY(copyItem && copyItem->property("enabled").toBool());
    QGuiApplication::clipboard()->setText(QStringLiteral("before"));
    QVERIFY(QMetaObject::invokeMethod(copyItem, "triggered"));
    QCOMPARE(QGuiApplication::clipboard()->text(), answer);
    QVERIFY(QMetaObject::invokeMethod(menu, "close"));
    // Copying only inline code in a list must not export the list's paragraph/indentation.
    const QString path = QStringLiteral("/Users/test/project");
    view.resetMessages(
        {textRow("inline", "assistant",
                 "Two checkouts:\n\n- SDK `" + path + "`, branch develop\n- Another checkout")},
        "workspace:inline-copy");
    QTRY_VERIFY(childNamed(delegateAt(transcript, 0), "messageBody"));
    body = childNamed(delegateAt(transcript, 0), "messageBody");
    const auto quickDocument = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
    QVERIFY(quickDocument);
    QTRY_VERIFY(quickDocument->textDocument()->toPlainText().contains(path));
    const int codeStart = quickDocument->textDocument()->toPlainText().indexOf(path);
    QVERIFY(QMetaObject::invokeMethod(body, "select", Q_ARG(int, codeStart),
                                      Q_ARG(int, codeStart + path.size())));
    QCOMPARE(body->property("selectedText").toString(), path);
    body->forceActiveFocus();
    QTest::keySequence(window, QKeySequence::Copy);
    QCOMPARE(QGuiApplication::clipboard()->text(), path);
    menu->setProperty("selectionItem", QVariant::fromValue<QObject*>(body));
    QGuiApplication::clipboard()->setText("before-context-menu-copy");
    QVERIFY(QMetaObject::invokeMethod(copyItem, "triggered"));
    QCOMPARE(QGuiApplication::clipboard()->text(), path);
    auto* prompt = window->rootObject()->findChild<QQuickItem*>("agentPrompt");
    QVERIFY(prompt);
    prompt->setProperty("text", "");
    prompt->forceActiveFocus();
    QTest::keySequence(window, QKeySequence::Paste);
    QCOMPARE(prompt->property("text").toString(), path);
    const QString indented = QStringLiteral("  alpha\n    beta");
    view.resetMessages({textRow("fenced", "assistant", "```text\n" + indented + "\n```")},
                       "workspace:indented-copy");
    QTRY_VERIFY(childNamed(delegateAt(transcript, 0), "messageBody"));
    body = childNamed(delegateAt(transcript, 0), "messageBody");
    const auto fenced = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
    QVERIFY(fenced);
    QTRY_VERIFY(fenced->textDocument()->toPlainText().contains(indented));
    const int start = fenced->textDocument()->toPlainText().indexOf(indented);
    QVERIFY(QMetaObject::invokeMethod(body, "select", Q_ARG(int, start),
                                      Q_ARG(int, start + indented.size())));
    body->forceActiveFocus();
    QTest::keySequence(window, QKeySequence::Copy);
    QCOMPARE(QGuiApplication::clipboard()->text(), indented);
}
