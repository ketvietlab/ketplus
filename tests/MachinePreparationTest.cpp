#include "FakeMachineBackend.h"
#include "MachineHosts.h"
#include "machine/MachineAttachmentPanel.h"
#include "machine/MachineCatalog.h"
#include "machine/MachineWorkspaceView.h"
#include <QDateTime>
#include <QDir>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest>
#include <memory>

using namespace ketplus;
using namespace ketplus::test;
class MachinePreparationTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
    static QJsonObject prepared() {
        return {{"ok", true},
                {"session_id", "private-draft"},
                {"session_generation", 1},
                {"expires_at_ms", QDateTime::currentMSecsSinceEpoch() + 60000}};
    }
    static FakeMachineBackend::Answer answers(bool* failPrepare = nullptr,
                                              bool* failTurn = nullptr) {
        return [base = sshHost(), failPrepare, failTurn](const FakeMachineBackend::Call& call,
                                                         QJsonObject* result, QString* error) {
            base(call, result, error);
            if (call.operation == "prepareSession") {
                if (failPrepare && *failPrepare)
                    *error = "Preparation refused";
                else
                    *result = prepared();
            } else if (call.operation == "attachment.stage.begin")
                *result = {{"revision", 1}};
            else if (call.operation == "attachment.stage.chunk")
                *result = {{"accepted", true}};
            else if (call.operation == "attachment.stage.commit")
                *result = {{"status", "ready"}};
            else if (call.operation == "startTurn") {
                if (failTurn && *failTurn)
                    *error = "First turn refused";
                else
                    *result = {{"ok", true}, {"turn_sequence", 1}};
            }
        };
    }
  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("dev.ketplus.tests.first-attachment");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        QVERIFY(QSettings().fileName().startsWith(settings_.path()));
        QVERIFY(!QSettings().contains("workspace/lastRoot"));
    }
    void firstTurnAttachmentsRetainDraftAcrossFailure_data() {
        QTest::addColumn<bool>("relay");
        QTest::newRow("ssh") << false;
        QTest::newRow("relay") << true;
    }
    void firstTurnAttachmentsRetainDraftAcrossFailure() {
        QFETCH(bool, relay);
        FakeMachineBackend backend("machine", relay ? Route::Relay : Route::Ssh);
        backend.features = {remote::MachineBackend::Feature::Attachments,
                            remote::MachineBackend::Feature::SessionPreparation};
        bool failPrepare = true, failTurn = true;
        backend.answer = answers(&failPrepare, &failTurn);
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        view.resize(1000, 760);
        view.show();
        catalog.setBackend(&backend);
        QTRY_VERIFY(!catalog.repositories().isEmpty());
        view.startNewConversation();
        auto* input = view.findChild<QPlainTextEdit*>("machineComposer");
        auto* send = view.findChild<QPushButton*>("machineSend");
        QVERIFY(input && send);
        input->setPlainText("Read this file");
        QTemporaryFile file;
        QVERIFY(file.open());
        file.write("FIRST_ATTACHMENT_GUI_REFERENCE");
        file.flush();
        view.attachFiles({file.fileName()});
        QCOMPARE(backend.calls("prepareSession").size(), 1);
        QVERIFY(!send->isEnabled());
        const auto requestId =
            backend.calls("prepareSession").first().request.value("client_request_id");
        failPrepare = false;
        view.attachFiles({}); // Retry keeps the request identity and selected local file.
        QCOMPARE(backend.calls("prepareSession").size(), 2);
        QCOMPARE(backend.calls("prepareSession").last().request.value("client_request_id"),
                 requestId);
        auto* panel = view.findChild<MachineAttachmentPanel*>();
        QVERIFY(panel);
        QTRY_VERIFY(panel->allReady() && panel->hasAttachments());
        QVERIFY(view.openSessionId().isEmpty());
        QCOMPARE(backend.calls("transcriptWindow").size(), 0);
        QCOMPARE(backend.calls("createSession").size(), 0);
        QTest::mouseClick(send, Qt::LeftButton);
        QCOMPARE(backend.calls("startTurn").size(), 1);
        QCOMPARE(backend.calls("startTurn").first().sessionId, "private-draft");
        QCOMPARE(
            backend.calls("startTurn").first().request.value("attachment_ids").toArray().size(), 1);
        QVERIFY(view.openSessionId().isEmpty());
        QCOMPARE(input->toPlainText(), "Read this file");
        QVERIFY(panel->hasAttachments());
        if (relay && !qEnvironmentVariable("KETPLUS_TEST_CAPTURE_DIR").isEmpty()) {
            QDir captures(qEnvironmentVariable("KETPLUS_TEST_CAPTURE_DIR"));
            QVERIFY(captures.mkpath("."));
            QVERIFY(view.grab().save(captures.filePath("first-turn-failure.png")));
        }
        failTurn = false;
        QTest::mouseClick(send, Qt::LeftButton);
        QTRY_COMPARE(view.openSessionId(), "private-draft");
        QVERIFY(input->toPlainText().isEmpty());
        QCOMPARE(backend.calls("createSession").size(), 0);
        QCOMPARE(backend.calls("cancelPreparedSession").size(), 0);
    }
    void leavingCancelsAnAllocationEvenIfPreparationCompletesLate() {
        FakeMachineBackend backend("machine", Route::Relay);
        backend.features = {remote::MachineBackend::Feature::Attachments,
                            remote::MachineBackend::Feature::SessionPreparation};
        backend.answer = answers();
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&backend);
        QTRY_VERIFY(!catalog.repositories().isEmpty());
        view.startNewConversation();
        QTemporaryFile file;
        QVERIFY(file.open());
        file.write("reference");
        file.flush();
        backend.hold = true;
        view.attachFiles({file.fileName()});
        view.startNewConversation();
        backend.hold = false;
        backend.release();
        QTRY_COMPARE(backend.calls("cancelPreparedSession").size(), 1);
        QCOMPARE(backend.calls("attachment.stage.begin").size(), 0);
        QVERIFY(view.openSessionId().isEmpty());
    }
    void queuedPreparationIsCancelledEvenAfterTheViewIsDestroyed() {
        FakeMachineBackend backend("machine", Route::Relay);
        backend.features = {remote::MachineBackend::Feature::Attachments,
                            remote::MachineBackend::Feature::SessionPreparation};
        backend.answer = [base = answers()](const FakeMachineBackend::Call& call,
                                            QJsonObject* result, QString* error) {
            base(call, result, error);
            if (call.operation == "prepareSession")
                *result = {{"status", "queued"}, {"command_id", "queued-preparation"}};
        };
        MachineCatalog catalog;
        auto view = std::make_unique<MachineWorkspaceView>(&catalog);
        catalog.setBackend(&backend);
        QTRY_VERIFY(!catalog.repositories().isEmpty());
        view->startNewConversation();
        QTemporaryFile file;
        QVERIFY(file.open());
        file.write("reference");
        file.flush();
        view->attachFiles({file.fileName()});
        view.reset();
        backend.complete("queued-preparation", prepared());
        QTRY_COMPARE(backend.calls("cancelPreparedSession").size(), 1);
        QCOMPARE(backend.calls("attachment.stage.begin").size(), 0);
        backend.complete("queued-preparation", prepared());
        QCOMPARE(backend.calls("cancelPreparedSession").size(), 1);
    }
    void removingTheLastFileReturnsToOrdinaryConversationCreation() {
        FakeMachineBackend backend("machine", Route::Ssh);
        backend.features = {remote::MachineBackend::Feature::Attachments,
                            remote::MachineBackend::Feature::SessionPreparation};
        backend.answer = answers();
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&backend);
        QTRY_VERIFY(!catalog.repositories().isEmpty());
        view.startNewConversation();
        QTemporaryFile file;
        QVERIFY(file.open());
        file.write("reference");
        file.flush();
        view.attachFiles({file.fileName()});
        auto* panel = view.findChild<MachineAttachmentPanel*>();
        QVERIFY(panel);
        QTRY_VERIFY(panel->allReady());
        auto* remove = panel->findChild<QPushButton*>();
        QVERIFY(remove);
        QTest::mouseClick(remove, Qt::LeftButton);
        QTRY_VERIFY(!panel->hasAttachments() && panel->allReady());
        view.findChild<QPlainTextEdit*>("machineComposer")->setPlainText("A plain first message");
        QTest::mouseClick(view.findChild<QPushButton*>("machineSend"), Qt::LeftButton);
        QCOMPARE(backend.calls("cancelPreparedSession").size(), 1);
        QCOMPARE(backend.calls("createSession").size(), 1);
        QCOMPARE(backend.calls("startTurn").size(), 0);
    }
    void oldDaemonKeepsFirstTurnAttachmentsUnavailable() {
        FakeMachineBackend backend("machine", Route::Ssh);
        backend.features = {remote::MachineBackend::Feature::Attachments};
        backend.answer = answers();
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&backend);
        QTRY_VERIFY(!catalog.repositories().isEmpty());
        view.startNewConversation();
        QVERIFY(!view.findChild<QPushButton*>("machineAttach")->isEnabled());
        view.attachFiles({"unused-synthetic-file.txt"});
        QCOMPARE(backend.calls("prepareSession").size(), 0);
    }
};
QTEST_MAIN(MachinePreparationTest)
#include "MachinePreparationTest.moc"
