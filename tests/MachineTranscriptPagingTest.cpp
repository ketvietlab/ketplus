#include "FakeMachineBackend.h"
#include "MachineHosts.h"
#include "machine/MachineCatalog.h"
#include "machine/MachineConversationActions.h"
#include "machine/MachineConversationSession.h"
#include "machine/MachineWorkspaceView.h"

#include <QAction>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QtTest>

using namespace ketplus;
using namespace ketplus::test;

namespace {

using Feature = remote::MachineBackend::Feature;

// The large assistant message of the middle page, sent as a reference and read in fragments.
QJsonObject largePayload() {
    return {
        {"item", QJsonObject{{"type", "agentMessage"},
                             {"id", "big"},
                             {"text", QString(3000, QLatin1Char('x')) + QStringLiteral(" end")}}}};
}

QByteArray largeBytes() { return QJsonDocument(largePayload()).toJson(QJsonDocument::Compact); }

QJsonObject largeReference() {
    return {{"version", 1},
            {"encoding", "json-utf8"},
            {"transfer_id", "t-big"},
            {"event_type", "item.completed"},
            {"item_id", "big"},
            {"digest_sha256",
             QString::fromLatin1(
                 QCryptographicHash::hash(largeBytes(), QCryptographicHash::Sha256).toHex())},
            {"total_bytes", largeBytes().size()},
            {"revision", 1}};
}

QJsonObject message(const QString& session, qint64 generation, qint64 cursor, const QString& text) {
    QJsonObject event = journalEvent(session, cursor, "user.message", {{"text", text}});
    event.insert("session_generation", generation);
    return event;
}

// Three windows of one conversation, newest first: 7-9, 4-6 (with the large event at 5) and
// 1-3. `large` sends cursor 5 as a reference (the local protocol over SSH); without it the
// route delivers it whole (the Relay reassembles before delivery).
FakeMachineBackend::Answer pagedHost(FakeMachineBackend::Answer base, const QString& session,
                                     qint64 generation, bool large, bool corrupt = false) {
    return [=](const FakeMachineBackend::Call& call, QJsonObject* result, QString* error) {
        base(call, result, error);
        const auto page = [&](qint64 first, bool more) {
            QJsonArray events;
            for (qint64 cursor = first; cursor < first + 3; ++cursor) {
                if (cursor == 5) {
                    QJsonObject event = journalEvent(
                        session, 5, "item.completed",
                        large ? QJsonObject{{"large_event", largeReference()}} : largePayload());
                    event.insert("session_generation", generation);
                    events.append(event);
                } else {
                    events.append(message(session, generation, cursor,
                                          QStringLiteral("message %1").arg(cursor)));
                }
            }
            *result = {{"events", events},
                       {"has_more_before", more},
                       {"snapshot_cursor", 9},
                       {"before_cursor", first},
                       {"first_event_cursor", first}};
        };
        if (call.operation == "transcriptWindow" && !call.request.contains("before_cursor"))
            page(7, true);
        else if (call.operation == "transcriptPage" ||
                 (call.operation == "transcriptWindow" && call.request.contains("before_cursor")))
            page(call.request.value("before_cursor").toInteger() == 7 ? 4 : 1,
                 call.request.value("before_cursor").toInteger() == 7);
        else if (call.operation == "transcriptLargeEvent") {
            const QByteArray bytes = largeBytes();
            const qint64 offset = call.request.value("offset").toInteger();
            const qint64 size = std::min<qint64>(bytes.size() / 2 + 1, bytes.size() - offset);
            QByteArray data = bytes.mid(offset, size);
            if (corrupt && offset > 0)
                data[0] = 'y';
            *result = {{"large_event_fragment_version", 1},
                       {"session_id", session},
                       {"session_generation", generation},
                       {"transfer_id", "t-big"},
                       {"event_type", "item.completed"},
                       {"item_id", "big"},
                       {"revision", 1},
                       {"digest_sha256", largeReference().value("digest_sha256")},
                       {"total_bytes", bytes.size()},
                       {"offset", offset},
                       {"next_offset", offset + data.size()},
                       {"complete", offset + data.size() == bytes.size()},
                       {"data", QString::fromLatin1(data.toBase64())}};
        }
    };
}

QStringList expectedRows() {
    QStringList rows;
    for (int cursor = 1; cursor <= 9; ++cursor)
        rows.append(cursor == 5 ? largePayload().value("item").toObject().value("text").toString()
                                : QStringLiteral("message %1").arg(cursor));
    return rows;
}

void scrollThroughHistory(FakeMachineBackend& backend, const QString& session) {
    MachineCatalog catalog;
    MachineWorkspaceView view(&catalog);
    catalog.setBackend(&backend);
    QTRY_COMPARE(catalog.conversations().size(), 1);
    view.openConversation(session);
    QTRY_COMPARE(transcriptRows(view), expectedRows().mid(6));
    auto* older = view.findChild<QPushButton*>("machineLoadOlder");
    QVERIFY(older->isVisibleTo(&view));
    // Rendering alone never pages.
    QCoreApplication::processEvents();
    QVERIFY(backend.calls("transcriptPage").isEmpty());

    // The reader scrolls to the top: the page before it is read.
    view.findChild<QListWidget*>("machineTranscript")
        ->verticalScrollBar()
        ->triggerAction(QAbstractSlider::SliderToMinimum);
    QTRY_COMPARE(transcriptRows(view), expectedRows().mid(3));
    older->click();
    QTRY_COMPARE(transcriptRows(view), expectedRows());
    // The last page said there is nothing before it.
    QVERIFY(!older->isVisibleTo(&view));
    QVERIFY(!view.session()->hasOlder());
    const auto pages = backend.calls("transcriptPage");
    QCOMPARE(pages.last().request.value("before_cursor").toInteger(), 4);
    // The fallback renderer says what it is.
    QVERIFY(view.findChild<QLabel*>("machineTranscriptNote")->isVisibleTo(&view));
}

} // namespace

class MachineTranscriptPagingTest final : public QObject {
    Q_OBJECT

  private slots:
    // T-18: scrolling up reads older windows through MachineBackend, newest first and with
    // nothing lost or repeated; the large event of the middle page is reassembled from its
    // fragments before it renders.
    void sshPagesOlderWindowsAndReassemblesLargeEvents() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.answer = pagedHost(sshHost(), "linux-1", 1, true);
        scrollThroughHistory(ssh, "linux-1");
        const auto fragments = ssh.calls("transcriptLargeEvent");
        QCOMPARE(fragments.size(), 2);
        QCOMPARE(fragments.first().request.value("offset").toInteger(), 0);
        QCOMPARE(fragments.first().request.value("transfer_id").toString(), "t-big");
        QVERIFY(fragments.last().request.value("offset").toInteger() > 0);
        QCOMPARE(ssh.calls("transcriptPage").first().request.value("before_cursor").toInteger(), 7);
    }

    void relayPagesOlderWindowsTheSameWay() {
        FakeMachineBackend relay("fp-relay", Route::Relay);
        bool verified = true;
        relay.answer = pagedHost(relayHost(&verified), "relay-1", 4, false);
        scrollThroughHistory(relay, "relay-1");
        QVERIFY(relay.calls("transcriptLargeEvent").isEmpty());
    }

    // A large event that does not reassemble is never rendered in part: the page is not
    // added, the reason shows, and the cached transcript stays.
    void aCorruptLargeEventAddsNothingAndSaysWhy() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.answer = pagedHost(sshHost(), "linux-1", 1, true, true);
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        view.openConversation("linux-1");
        QTRY_COMPARE(transcriptRows(view).size(), 3);
        view.findChild<QPushButton*>("machineLoadOlder")->click();
        QTRY_VERIFY(!view.session()->lastError().isEmpty());
        QCOMPARE(transcriptRows(view), expectedRows().mid(6));
        QVERIFY(!view.findChild<QLabel*>("machineComposerReason")->text().isEmpty());
        QVERIFY(view.session()->hasOlder()); // scrolling up again retries
    }

    // Live events that arrive while the window loads follow it instead of being lost with the
    // old rows, and a live item sent as a large_event reference renders once it is read.
    void liveEventsDuringALoadAndLargeLiveItemsAreKept() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.answer = pagedHost(sshHost(), "linux-1", 1, true);
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 1);

        ssh.hold = true;
        view.openConversation("linux-1");
        QCOMPARE(ssh.calls("transcriptWindow").size(), 1);
        ssh.deliver(message("linux-1", 1, 10, "live during load"));
        QJsonObject large =
            journalEvent("linux-1", 11, "item.completed", {{"large_event", largeReference()}});
        ssh.deliver(large);
        ssh.deliver(message("linux-1", 1, 12, "after the large item"));
        QVERIFY(transcriptRows(view).isEmpty());
        ssh.hold = false;
        ssh.release();

        QStringList expected = expectedRows().mid(6);
        expected << "live during load"
                 << largePayload().value("item").toObject().value("text").toString()
                 << "after the large item";
        QTRY_COMPARE(transcriptRows(view), expected);
        QCOMPARE(ssh.calls("transcriptLargeEvent").size(), 2);
        QVERIFY(view.session()->lastError().isEmpty());
    }

    // A reader looking at older rows is not pulled to the newest one by a live event; one at
    // the newest row follows it.
    void aReaderInOlderRowsKeepsTheirPlace() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.answer = pagedHost(sshHost(), "linux-1", 1, false);
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        view.resize(320, 260);
        view.show();
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        view.openConversation("linux-1");
        QTRY_COMPARE(transcriptRows(view).size(), 3);
        auto* older = view.findChild<QPushButton*>("machineLoadOlder");
        older->click();
        QTRY_COMPARE(transcriptRows(view).size(), 6);
        older->click();
        QTRY_COMPARE(transcriptRows(view).size(), 9);
        auto* list = view.findChild<QListWidget*>("machineTranscript");
        QScrollBar* bar = list->verticalScrollBar();
        QTRY_VERIFY(bar->maximum() > 0);

        bar->setValue(0);
        ssh.deliver(message("linux-1", 1, 10, "live 10"));
        QTRY_COMPARE(transcriptRows(view).size(), 10);
        QCoreApplication::processEvents();
        QCOMPARE(bar->value(), 0);

        bar->setValue(bar->maximum());
        ssh.deliver(message("linux-1", 1, 11, "live 11"));
        QTRY_COMPARE(transcriptRows(view).size(), 11);
        QTRY_COMPARE(bar->value(), bar->maximum());
    }

    // T-18: rename, fork and archive go through MachineBackend where the route carries them;
    // elsewhere they stay in the menu, disabled with the reason. Attachments say why too.
    void conversationActionsFollowTheRoute() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.features = {Feature::Rename, Feature::Fork};
        auto base = sshHost();
        QString forkId;
        ssh.answer = [&](const FakeMachineBackend::Call& call, QJsonObject* result,
                         QString* error) {
            base(call, result, error);
            if (call.operation == "forkConversation") {
                forkId = call.request.value("fork_session_id").toString();
                *result = {{"session_id", forkId}, {"session_generation", 1}};
            }
            // The machine lists the fork once it exists.
            if (call.operation == "listConversations" && !forkId.isEmpty()) {
                QJsonArray rows = result->value("sessions").toArray();
                QJsonObject row = rows.first().toObject();
                row.insert("session_id", forkId);
                row.insert("title", "Fork");
                rows.append(row);
                *result = {{"sessions", rows}};
            }
        };
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        view.openConversation("linux-1");

        std::unique_ptr<QMenu> menu(view.actions()->menu("linux-1", &view));
        auto* rename = menu->findChild<QAction*>("machineRenameAction");
        auto* fork = menu->findChild<QAction*>("machineForkAction");
        auto* archive = menu->findChild<QAction*>("machineArchiveAction");
        QVERIFY(rename != nullptr && fork != nullptr && archive != nullptr);
        QVERIFY(rename->isEnabled() && fork->isEnabled());
        QVERIFY(!archive->isEnabled());
        QCOMPARE(archive->toolTip(), QStringLiteral("Not available on Linux machines yet."));

        auto* attach = view.findChild<QPushButton*>("machineAttach");
        QVERIFY(attach->isVisibleTo(&view));
        QVERIFY(!attach->isEnabled());
        QCOMPARE(attach->toolTip(), QStringLiteral("Not available on Linux machines yet."));

        const auto listings = ssh.calls("listConversations").size();
        view.actions()->rename("linux-1", "  Release notes ");
        QCOMPARE(ssh.calls("renameConversation").size(), 1);
        QCOMPARE(ssh.calls("renameConversation").first().request.value("title").toString(),
                 "Release notes");
        QCOMPARE(ssh.calls("renameConversation").first().generation, 1);
        QTRY_VERIFY(ssh.calls("listConversations").size() > listings); // the list refreshes

        fork->trigger();
        QCOMPARE(ssh.calls("forkConversation").size(), 1);
        QCOMPARE(forkId,
                 ssh.calls("forkConversation").first().request.value("fork_session_id").toString());
        QVERIFY(!forkId.isEmpty());
        QTRY_COMPARE(view.openSessionId(), forkId); // the fork opens

        // Archive is refused before anything is sent, with the reason in view.
        view.actions()->archive(forkId);
        QVERIFY(ssh.calls("setConversationArchived").isEmpty());
        QCOMPARE(view.actions()->lastError(),
                 QStringLiteral("Not available on Linux machines yet."));
        QCOMPARE(view.findChild<QLabel*>("machineComposerReason")->text(),
                 QStringLiteral("Not available on Linux machines yet."));

        // A failed rename is reported, not dropped.
        ssh.answer = [](const FakeMachineBackend::Call&, QJsonObject*, QString* error) {
            *error = QStringLiteral("The title is too long.");
        };
        view.actions()->rename(forkId, "x");
        QCOMPARE(view.actions()->lastError(), QStringLiteral("The title is too long."));

        // Offline, every action is disabled with the connection's reason.
        ssh.setState(State::Offline, "The SSH connection closed.");
        std::unique_ptr<QMenu> offline(view.actions()->menu(forkId, &view));
        auto* offlineRename = offline->findChild<QAction*>("machineRenameAction");
        QVERIFY(!offlineRename->isEnabled());
        QCOMPARE(offlineRename->toolTip(),
                 QStringLiteral("Available once this machine is connected."));
    }

    // A queued Relay rename finishes when the host confirms it.
    void aQueuedRelayRenameFinishesOnCompletion() {
        FakeMachineBackend relay("fp-relay", Route::Relay);
        relay.features = {Feature::Rename};
        bool verified = true;
        auto base = relayHost(&verified);
        relay.answer = [&](const FakeMachineBackend::Call& call, QJsonObject* result,
                           QString* error) {
            base(call, result, error);
            if (call.operation == "renameConversation")
                *result = {{"command_id", "cmd-1"}, {"status", "queued"}};
        };
        MachineCatalog catalog;
        MachineConversationActions actions(&catalog);
        catalog.setBackend(&relay);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        QString reason;
        QVERIFY(!actions.available(Feature::Fork, &reason));
        QVERIFY(!reason.isEmpty());
        QSignalSpy finished(&actions, &MachineConversationActions::finished);
        actions.rename("relay-1", "Deploy notes");
        QCOMPARE(relay.calls("renameConversation").first().generation, 4);
        QCOMPARE(finished.size(), 0); // queued: nothing is final yet
        QCOMPARE(actions.pendingStatus(), QStringLiteral("Rename sent. Waiting for the machine…"));
        relay.complete("cmd-1", {}, "The host refused the title.");
        QTRY_VERIFY(!finished.isEmpty());
        QCOMPARE(actions.lastError(), QStringLiteral("The host refused the title."));
        QVERIFY(actions.pendingStatus().isEmpty());
    }

    // A fork the Relay queued is never silent: the status line says it waits for the machine
    // until the host answers, then shows the host's error.
    void aQueuedRelayForkSaysItWaitsThenWhyItFailed() {
        FakeMachineBackend relay("fp-relay", Route::Relay);
        relay.features = {Feature::Fork};
        bool verified = true;
        auto base = relayHost(&verified);
        relay.answer = [&](const FakeMachineBackend::Call& call, QJsonObject* result,
                           QString* error) {
            base(call, result, error);
            if (call.operation == "forkConversation")
                *result = {{"command_id", "cmd-fork"}, {"status", "queued"}};
        };
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&relay);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        view.openConversation("relay-1");
        auto* reason = view.findChild<QLabel*>("machineComposerReason");

        view.actions()->fork("relay-1");
        QCOMPARE(relay.calls("forkConversation").size(), 1);
        QCOMPARE(reason->text(), QStringLiteral("Fork sent. Waiting for the machine…"));
        QCOMPARE(view.openSessionId(), QStringLiteral("relay-1"));

        relay.complete("cmd-fork", {}, "The host could not fork this conversation.");
        QTRY_COMPARE(reason->text(), QStringLiteral("The host could not fork this conversation."));
        QVERIFY(view.actions()->pendingStatus().isEmpty());
        QCOMPARE(view.openSessionId(), QStringLiteral("relay-1"));
    }
};

QTEST_MAIN(MachineTranscriptPagingTest)
#include "MachineTranscriptPagingTest.moc"
