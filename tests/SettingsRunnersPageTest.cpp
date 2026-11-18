// Settings → Runners: the account's runners with their relation to this computer, the row
// actions behind their confirmations, and adding a runner with a pairing token that only a
// click approves. Run with QT_QPA_PLATFORM=offscreen.
#include "runners/SettingsRunnersPage.h"
#include "KetAppearance.h"
#include "KetBadge.h"
#include "KetBanner.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetEmptyState.h"
#include "KetTextField.h"
#include "app/SettingsDialog.h"
#include "app/SettingsWorkspacePage.h"
#include "runners/RunnerPairingDialog.h"
#include "runners/RunnerRowWidget.h"
#include "ui/Theme.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QScopeGuard>
#include <QtTest>

#include <functional>
#include <utility>

using namespace ketplus;
using runners::RowAction;

namespace {

const QString token = QStringLiteral("kpt1.SECRET-TOKEN-VALUE-0123456789");
const QString htmlName = QStringLiteral("<b>bold</b><a href=\"https://example.invalid\">x</a>");

QStringList captured;
QtMessageHandler previousHandler = nullptr;

void capture(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    captured.append(message);
    if (previousHandler)
        previousHandler(type, context, message);
}

QJsonObject runner(const QString& id, const QString& name, bool pinned, bool paired,
                   const QString& state = QStringLiteral("idle")) {
    return {
        {"runner_id", id},
        {"name", name},
        {"os", "linux"},
        {"arch", "x86_64"},
        {"labels", QJsonArray{"gpu", "<i>big</i>"}},
        {"state", state},
        {"runner_version", "1.2.3"},
        {"last_seen_at", QDateTime::currentDateTimeUtc().addSecs(-120).toString(Qt::ISODateWithMs)},
        {"pinned", pinned},
        {"paired_with_this_machine", paired}};
}

QJsonObject created(const QString& ticket = QStringLiteral("t1"), int expiresInMs = 3600000) {
    return {{"ok", true},
            {"ticket_id", ticket},
            {"state", "ticketed"},
            {"token", token},
            {"expires_at",
             QDateTime::currentDateTimeUtc().addMSecs(expiresInMs).toString(Qt::ISODateWithMs)}};
}

QJsonObject claimed(const QString& ticket = QStringLiteral("t1")) {
    return {{"ok", true},
            {"ticket_id", ticket},
            {"state", "claimed"},
            {"runner", QJsonObject{{"runner_id", "r-new"},
                                   {"name", htmlName},
                                   {"os", "linux"},
                                   {"arch", "aarch64"},
                                   {"labels", QJsonArray{"ci"}},
                                   {"fingerprint", "SHA256:abcd"}}},
            {"confirmation_code", "482-913"},
            {"attempts_left", 3}};
}

} // namespace

class SettingsRunnersPageTest final : public QObject {
    Q_OBJECT

    // Answers each type from `answers`; with `hold`, answers wait in `held` until answer().
    struct FakeDaemon {
        QList<QPair<QString, QJsonObject>> sent;
        QHash<QString, QPair<QJsonObject, QString>> answers;
        bool hold = false;
        QList<std::function<void()>> held;

        FakeDaemon() {
            answers.insert("runner.list",
                           {{{"ok", true},
                             {"runners", QJsonArray{runner("r1", "alpha", true, true),
                                                    runner("r2", "beta", false, false),
                                                    runner("r3", "gamma", true, false)}},
                             {"unlisted_pins", QJsonArray{}},
                             {"complete", true}},
                            {}});
            answers.insert("runner.pairing.create", {created(), {}});
            answers.insert("runner.pairing.status", {claimed(), {}});
            answers.insert("runner.pairing.approve",
                           {{{"ok", true}, {"ticket_id", "t1"}, {"state", "active"}}, {}});
            answers.insert("runner.pairing.reject",
                           {{{"ok", true}, {"ticket_id", "t1"}, {"state", "rejected"}}, {}});
            for (const char* type :
                 {"runner.disable", "runner.enable", "runner.unpair", "runner.remove"})
                answers.insert(type, {{{"ok", true}, {"runner_id", "r1"}}, {}});
            answers.insert("remote.status", {{{"ok", true}, {"configured", true}}, {}});
        }
        int count(const QString& type) const {
            int n = 0;
            for (const auto& [kind, payload] : sent)
                n += kind == type ? 1 : 0;
            return n;
        }
        QJsonObject last(const QString& type) const {
            for (auto it = sent.crbegin(); it != sent.crend(); ++it)
                if (it->first == type)
                    return it->second;
            return {};
        }
        void answer() {
            const auto waiting = std::exchange(held, {});
            for (const auto& one : waiting)
                one();
        }
        runners::Requester requester() {
            return [this](const QString& type, const QJsonObject& payload, runners::Reply reply) {
                sent.append({type, payload});
                const auto [result, error] = answers.value(type);
                if (hold) {
                    held.append([reply, result, error] { reply(result, error); });
                    return;
                }
                reply(result, error);
            };
        }
    };

    // Empty when every label holding markup characters shows them as plain text; otherwise
    // the first offender, for the failure message.
    static QString richLabel(QWidget* root) {
        for (QLabel* label : root->findChildren<QLabel*>())
            // The kit's empty state escapes its message itself and only holds the page's own
            // sentences.
            if (!qobject_cast<ket::EmptyState*>(label->parent()) &&
                label->textFormat() != Qt::PlainText && label->text().contains('<'))
                return label->objectName() + QLatin1Char(' ') +
                       QString::fromLatin1(label->parent()->metaObject()->className()) +
                       QLatin1Char(' ') + label->text();
        return {};
    }

    static runners::RunnerPairingDialog* startPairing(SettingsRunnersPage& page) {
        page.addRunnerButton()->click();
        return page.pairingDialog();
    }

  private slots:
    void initTestCase() { previousHandler = qInstallMessageHandler(capture); }
    void cleanupTestCase() { qInstallMessageHandler(previousHandler); }
    void init() { captured.clear(); }

    void cleanup() {
        // The token never reaches a log line, whatever the test did.
        for (const QString& line : std::as_const(captured))
            QVERIFY2(!line.contains(token), qPrintable(line));
    }

    void listsEveryRunnerWithItsRelation() {
        FakeDaemon daemon;
        SettingsRunnersPage page(daemon.requester());
        page.show();
        QCOMPARE(daemon.count("runner.list"), 1);
        QCOMPARE(page.rows().size(), 3);
        auto* trusted = page.row("r1");
        auto* notPaired = page.row("r2");
        auto* mismatch = page.row("r3");
        QCOMPARE(trusted->findChild<ket::Badge*>("runnerRelation")->text(),
                 QStringLiteral("Trusted"));
        QCOMPARE(notPaired->findChild<ket::Badge*>("runnerRelation")->text(),
                 QStringLiteral("Not paired"));
        QCOMPARE(mismatch->findChild<ket::Badge*>("runnerRelation")->text(),
                 QStringLiteral("Pairing mismatch"));
        QVERIFY(!trusted->findChild<ket::Banner*>("runnerMismatch"));
        QVERIFY(!notPaired->findChild<ket::Banner*>("runnerMismatch"));
        QVERIFY(mismatch->findChild<ket::Banner*>("runnerMismatch"));
        // Pair is offered to every runner that is not trusted, never to a trusted one.
        QVERIFY(!trusted->button(RowAction::Pair));
        QVERIFY(notPaired->button(RowAction::Pair));
        QVERIFY(mismatch->button(RowAction::Pair));
        QVERIFY(!notPaired->button(RowAction::Unpair));
        bool platform = false;
        for (QLabel* label : trusted->findChildren<QLabel*>())
            platform = platform || (label->text().contains("linux x86_64") &&
                                    label->text().contains("Version 1.2.3") &&
                                    label->text().contains("Seen 2 minutes ago"));
        QVERIFY(platform);
        QVERIFY(page.findChild<QLabel*>("settingsWorkspaceIntro")->text().contains("trusted"));
    }

    void relayTextStaysPlain() {
        FakeDaemon daemon;
        daemon.answers["runner.list"].first["runners"] =
            QJsonArray{runner("r1", htmlName, true, true, "<i>odd</i>")};
        SettingsRunnersPage page(daemon.requester());
        page.resize(800, 600);
        page.show();
        auto* row = page.row("r1");
        QVERIFY(row);
        QCOMPARE(row->displayName(), htmlName);
        QCOMPARE(row->findChild<ket::Badge*>("runnerState")->text(), QStringLiteral("<i>odd</i>"));
        QCOMPARE(richLabel(&page), QString());
        // The confirmation names the runner as it came, as plain text.
        row->button(RowAction::Remove)->click();
        QVERIFY(page.confirmDialog());
        QVERIFY(page.confirmDialog()->title().contains(htmlName));
        QCOMPARE(richLabel(page.confirmDialog()), QString());
        page.confirmDialog()->reject();
        QCOMPARE(daemon.count("runner.remove"), 0);
    }

    void brokenEntriesAreNeverTrusted() {
        FakeDaemon daemon;
        daemon.answers["runner.list"].first["runners"] = QJsonArray{
            QJsonObject{{"runner_id", "r1"}, {"pinned", "true"}, {"paired_with_this_machine", 1}},
            QJsonObject{{"name", "no id"}},
            QJsonObject{{"runner_id", "r2"},
                        {"pinned", true},
                        {"paired_with_this_machine", true},
                        {"trusted", false}},
            42};
        SettingsRunnersPage page(daemon.requester());
        page.show();
        QCOMPARE(page.rows().size(), 2);
        QCOMPARE(page.row("r1")->findChild<ket::Badge*>("runnerRelation")->text(),
                 QStringLiteral("Not paired"));
        QCOMPARE(page.row("r2")->findChild<ket::Badge*>("runnerRelation")->text(),
                 QStringLiteral("Pairing mismatch"));
    }

    void anEmptyListOffersToAddOne() {
        FakeDaemon daemon;
        daemon.answers["runner.list"].first["runners"] = QJsonArray{};
        SettingsRunnersPage page(daemon.requester());
        page.show();
        QVERIFY(page.emptyState()->isVisible());
        QCOMPARE(page.emptyState()->title(), QStringLiteral("No runners yet"));
        page.emptyState()->action()->click();
        QCOMPARE(daemon.count("runner.pairing.create"), 1);
        QVERIFY(page.pairingDialog());
    }

    void notesAndPinWarningsAreShown() {
        FakeDaemon daemon;
        daemon.answers["runner.list"].first["complete"] = false;
        daemon.answers["runner.list"].first["unlisted_pins"] = QJsonArray{"gone1", "gone2"};
        daemon.answers["runner.list"].first["pin_error"] = "<b>bad pin</b>";
        SettingsRunnersPage page(daemon.requester());
        page.show();
        QVERIFY(page.note().contains("only part"));
        QVERIFY(page.note().contains("2 runners"));
        QVERIFY(page.findChild<ket::Banner*>("runnersPinWarning")->isVisible());
    }

    void unavailableCasesHaveTheirOwnWords_data() {
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("error");
        QTest::addColumn<QJsonObject>("status");
        QTest::addColumn<QString>("title");
        const QJsonObject signedIn{{"ok", true}, {"configured", true}};
        QTest::newRow("not enrolled")
            << "not_enrolled" << "" << signedIn << "Sign in to use runners";
        QTest::newRow("relay") << "runner_unavailable" << "" << signedIn
                               << "Runners are not available";
        QTest::newRow("signed out")
            << "runner_unavailable" << "" << QJsonObject{{"ok", true}, {"configured", false}}
            << "Sign in to use runners";
        QTest::newRow("entitlement")
            << "entitlement_required" << "" << signedIn << "Runners are not included";
        QTest::newRow("old daemon")
            << "unknown_operation" << "" << signedIn << "Update KetPlus to use runners";
        QTest::newRow("no daemon")
            << "" << "Connection refused" << signedIn << "KetPlus Daemon is not running";
        QTest::newRow("network") << "network" << "" << signedIn << "Cannot reach KetPlus Relay";
    }
    void unavailableCasesHaveTheirOwnWords() {
        QFETCH(QString, code);
        QFETCH(QString, error);
        QFETCH(QJsonObject, status);
        QFETCH(QString, title);
        FakeDaemon daemon;
        QJsonObject failure{{"ok", false}};
        if (!code.isEmpty())
            failure.insert("error_code", code);
        daemon.answers["runner.list"] = {code.isEmpty() ? QJsonObject{} : failure,
                                         code.isEmpty() ? error : QStringLiteral("rejected")};
        daemon.answers["remote.status"] = {status, {}};
        SettingsRunnersPage page(daemon.requester());
        QSignalSpy account(&page, &SettingsRunnersPage::accountRequested);
        page.show();
        QVERIFY(page.emptyState()->isVisible());
        QCOMPARE(page.emptyState()->title(), title);
        QVERIFY(page.rows().isEmpty());
        QVERIFY(!page.addRunnerButton()->isEnabled());
        QCOMPARE(daemon.count("remote.status"), code == "runner_unavailable" ? 1 : 0);
        page.emptyState()->action()->click();
        if (title == "Sign in to use runners")
            QCOMPARE(account.size(), 1);
        else
            QCOMPARE(daemon.count("runner.list"), 2);
    }

    void anUnknownFailureShowsTheDaemonsWords() {
        FakeDaemon daemon;
        daemon.answers["runner.list"] = {
            {{"ok", false}, {"error_code", "odd_case"}, {"error_message", "<b>broken</b>"}},
            "rejected"};
        SettingsRunnersPage page(daemon.requester());
        page.show();
        QCOMPARE(page.error(), QStringLiteral("The request failed: <b>broken</b>"));
        QVERIFY(page.rows().isEmpty());
    }

    void disableAndEnableSendTheirTypeAndLockTheRow() {
        FakeDaemon daemon;
        daemon.answers["runner.list"].first["runners"] = QJsonArray{
            runner("r1", "alpha", true, true), runner("r2", "beta", true, true, "disabled")};
        SettingsRunnersPage page(daemon.requester());
        page.show();
        daemon.hold = true;
        page.row("r1")->button(RowAction::Disable)->click();
        QCOMPARE(daemon.last("runner.disable"), QJsonObject({{"runner_id", "r1"}}));
        QVERIFY(page.row("r1")->isBusy());
        QVERIFY(!page.row("r1")->button(RowAction::Disable)->isEnabled());
        page.row("r1")->button(RowAction::Disable)->click();
        QCOMPARE(daemon.count("runner.disable"), 1);
        daemon.hold = false;
        daemon.answer();
        QCOMPARE(daemon.count("runner.list"), 2);
        QVERIFY(!page.row("r1")->isBusy());
        QVERIFY(!page.row("r2")->button(RowAction::Disable));
        page.row("r2")->button(RowAction::Enable)->click();
        QCOMPARE(daemon.last("runner.enable"), QJsonObject({{"runner_id", "r2"}}));
    }

    void unpairAndRemoveAskFirst_data() {
        QTest::addColumn<int>("action");
        QTest::addColumn<QString>("type");
        QTest::newRow("unpair") << int(RowAction::Unpair) << "runner.unpair";
        QTest::newRow("remove") << int(RowAction::Remove) << "runner.remove";
    }
    void unpairAndRemoveAskFirst() {
        QFETCH(int, action);
        QFETCH(QString, type);
        FakeDaemon daemon;
        SettingsRunnersPage page(daemon.requester());
        page.resize(800, 600);
        page.show();
        auto* button = page.row("r1")->button(RowAction(action));
        button->click();
        QVERIFY(page.confirmDialog() && page.confirmDialog()->isOpen());
        QVERIFY(page.confirmDialog()->title().contains("alpha"));
        QCOMPARE(daemon.count(type), 0);
        // Esc and Cancel send nothing.
        QTest::keyClick(page.confirmDialog(), Qt::Key_Escape);
        QCOMPARE(daemon.count(type), 0);
        QTRY_VERIFY(!page.confirmDialog());
        page.row("r1")->button(RowAction(action))->click();
        page.confirmDialog()->findChild<ket::Button*>("runnersConfirmCancel")->click();
        QCOMPARE(daemon.count(type), 0);
        QTRY_VERIFY(!page.confirmDialog());
        page.row("r1")->button(RowAction(action))->click();
        QTest::keyClick(page.confirmDialog(), Qt::Key_Return);
        QCOMPARE(daemon.count(type), 0);
        page.confirmDialog()->findChild<ket::Button*>("runnersConfirmAction")->click();
        QCOMPARE(daemon.count(type), 1);
        QCOMPARE(daemon.last(type), QJsonObject({{"runner_id", "r1"}}));
        QCOMPARE(daemon.count("runner.list"), 2);
    }

    void aFailedActionIsShownAndTheListRefreshed() {
        FakeDaemon daemon;
        daemon.answers["runner.disable"] = {{{"ok", false}, {"error_code", "runner_not_found"}},
                                            "rejected"};
        SettingsRunnersPage page(daemon.requester());
        page.show();
        page.row("r1")->button(RowAction::Disable)->click();
        QCOMPARE(page.error(),
                 QStringLiteral("This runner is no longer in your account. Refresh the list."));
        QCOMPARE(daemon.count("runner.list"), 2);
    }

    void pairingShowsTheTokenMaskedAndOnlyAClickApproves() {
        FakeDaemon daemon;
        SettingsRunnersPage page(daemon.requester());
        page.resize(900, 700);
        page.show();
        daemon.hold = true;
        page.addRunnerButton()->click();
        page.addRunnerButton()->click();
        QCOMPARE(daemon.count("runner.pairing.create"), 1);
        QCOMPARE(daemon.last("runner.pairing.create"), QJsonObject{});
        daemon.hold = false;
        daemon.answer();
        auto* pairing = page.pairingDialog();
        QVERIFY(pairing);
        auto* field = pairing->tokenField();
        QCOMPARE(field->text(), token);
        QCOMPARE(field->echoMode(), QLineEdit::Password);
        QVERIFY(field->isReadOnly());
        const auto* command = pairing->dialog()->findChild<QLabel*>("runnerPairingCommand");
        QCOMPARE(command->text(), QStringLiteral("ketplus-runner register -"));
        QVERIFY(!pairing->dialog()->findChild<QLabel*>("runnerPairingExpiry")->text().isEmpty());
        QVERIFY(!pairing->approveButton()->isVisible());
        // Copying needs a click, and copies the token itself.
        QGuiApplication::clipboard()->clear();
        pairing->copyTokenButton()->click();
        QCOMPARE(QGuiApplication::clipboard()->text(), token);
        QGuiApplication::clipboard()->clear();

        // Claimed: the runner and its code are shown; nothing approves on its own.
        for (int i = 0; i < 3; ++i)
            page.pairingChanged({{"ticket_id", "t1"}, {"state", "claimed"}});
        QCOMPARE(daemon.count("runner.pairing.status"), 3);
        QCOMPARE(daemon.count("runner.pairing.approve"), 0);
        QVERIFY(field->text().isEmpty());
        QCOMPARE(pairing->dialog()->findChild<QLabel*>("runnerPairingCode")->text(),
                 QStringLiteral("482-913"));
        QCOMPARE(pairing->dialog()->findChild<QLabel*>("runnerPairingName")->text(), htmlName);
        QCOMPARE(richLabel(pairing->dialog()), QString());
        QVERIFY(pairing->approveButton()->isVisible());
        QVERIFY(pairing->approveButton()->isEnabled());
        QTest::keyClick(pairing->dialog(), Qt::Key_Return);
        QTest::keyClick(pairing->dialog(), Qt::Key_Enter);
        pairing->approveButton()->setFocus();
        QTest::keyClick(pairing->approveButton(), Qt::Key_Return);
        QCOMPARE(daemon.count("runner.pairing.approve"), 0);
        // An event for another ticket reads nothing.
        page.pairingChanged({{"ticket_id", "other"}});
        QCOMPARE(daemon.count("runner.pairing.status"), 3);

        daemon.hold = true;
        pairing->approveButton()->click();
        pairing->approveButton()->click();
        QCOMPARE(daemon.count("runner.pairing.approve"), 1);
        QCOMPARE(daemon.last("runner.pairing.approve"),
                 QJsonObject({{"ticket_id", "t1"}, {"code", "482-913"}}));
        daemon.hold = false;
        daemon.answer();
        QVERIFY(pairing->isFinished());
        QVERIFY(pairing->message().contains("paired"));
        pairing->rejectButton()->click(); // now "Close"
        QCOMPARE(daemon.count("runner.pairing.reject"), 0);
        QTRY_VERIFY(!page.pairingDialog());
    }

    void approveNeedsAClaimAndShowsAttemptsLeft() {
        FakeDaemon daemon;
        SettingsRunnersPage page(daemon.requester());
        page.show();
        auto* pairing = startPairing(page);
        // Before the claim there is no approve to press.
        pairing->approveButton()->click();
        QCOMPARE(daemon.count("runner.pairing.approve"), 0);
        pairing->checkButton()->click();
        daemon.answers["runner.pairing.approve"] = {
            {{"ok", false}, {"error_code", "code_mismatch"}, {"attempts_left", 2}}, "rejected"};
        QJsonObject after = claimed();
        after["attempts_left"] = 2;
        daemon.answers["runner.pairing.status"] = {after, {}};
        const int reads = daemon.count("runner.pairing.status");
        pairing->approveButton()->click();
        QCOMPARE(pairing->error(),
                 QStringLiteral("The confirmation code did not match. 2 attempts left."));
        QCOMPARE(daemon.count("runner.pairing.status"), reads + 1);
        QCOMPARE(daemon.count("runner.pairing.approve"), 1);
    }

    void pairingAgainNamesTheRunner() {
        FakeDaemon daemon;
        SettingsRunnersPage page(daemon.requester());
        page.show();
        page.row("r2")->button(RowAction::Pair)->click();
        QCOMPARE(daemon.last("runner.pairing.create"), QJsonObject({{"runner_id", "r2"}}));
        QCOMPARE(page.pairingDialog()->dialog()->findChild<QLabel*>("runnerPairingCommand")->text(),
                 QStringLiteral("ketplus-runner pair -"));
        QVERIFY(page.pairingDialog()->dialog()->title().contains("beta"));
    }

    void closingBeforeTheEndRejectsOnce_data() {
        QTest::addColumn<QString>("how");
        QTest::newRow("escape") << "escape";
        QTest::newRow("cancel") << "cancel";
        QTest::newRow("hide") << "hide";
        QTest::newRow("claimed escape") << "claimed";
    }
    void closingBeforeTheEndRejectsOnce() {
        QFETCH(QString, how);
        FakeDaemon daemon;
        SettingsRunnersPage page(daemon.requester());
        page.resize(800, 600);
        page.show();
        auto* pairing = startPairing(page);
        QPointer<ket::TextField> field = pairing->tokenField();
        QCOMPARE(field->text(), token);
        if (how == "claimed")
            pairing->checkButton()->click();
        if (how == "escape" || how == "claimed")
            QTest::keyClick(pairing->dialog(), Qt::Key_Escape);
        else if (how == "cancel")
            pairing->rejectButton()->click();
        else
            page.hide();
        QVERIFY(!field || field->text().isEmpty());
        QCOMPARE(daemon.count("runner.pairing.reject"), 1);
        QCOMPARE(daemon.last("runner.pairing.reject"), QJsonObject({{"ticket_id", "t1"}}));
        QCOMPARE(daemon.count("runner.pairing.approve"), 0);
        if (how == "cancel") {
            QVERIFY(pairing->isFinished());
            pairing->rejectButton()->click();
        }
        QTRY_VERIFY(!page.pairingDialog());
        QCOMPARE(daemon.count("runner.pairing.reject"), 1);
    }

    void aTokenThatArrivesAfterCloseIsRejected() {
        FakeDaemon daemon;
        auto page = std::make_unique<SettingsRunnersPage>(daemon.requester());
        page->show();
        daemon.hold = true;
        page->addRunnerButton()->click();
        QTest::keyClick(page->pairingDialog()->dialog(), Qt::Key_Escape);
        QCOMPARE(daemon.count("runner.pairing.reject"), 0);
        page.reset();
        daemon.hold = false;
        daemon.answer();
        QCOMPARE(daemon.count("runner.pairing.reject"), 1);
        QCOMPARE(daemon.last("runner.pairing.reject"), QJsonObject({{"ticket_id", "t1"}}));
    }

    void theTokenLeavesWhenItExpires() {
        FakeDaemon daemon;
        daemon.answers["runner.pairing.create"] = {created("t1", 150), {}};
        daemon.answers["runner.pairing.status"] = {
            {{"ok", true}, {"ticket_id", "t1"}, {"state", "expired"}}, {}};
        SettingsRunnersPage page(daemon.requester());
        page.show();
        auto* pairing = startPairing(page);
        QCOMPARE(pairing->tokenField()->text(), token);
        QTRY_VERIFY(pairing->tokenField()->text().isEmpty());
        QCOMPARE(daemon.count("runner.pairing.status"), 1);
        QVERIFY(pairing->isFinished());
        QVERIFY(pairing->message().contains("expired"));
        QTest::keyClick(pairing->dialog(), Qt::Key_Escape);
        QCOMPARE(daemon.count("runner.pairing.reject"), 0);
    }

    void lateRepliesAfterThePageIsGoneDoNothing() {
        FakeDaemon daemon;
        auto page = std::make_unique<SettingsRunnersPage>(daemon.requester());
        page->show();
        daemon.hold = true;
        page->refresh();
        page->row("r1")->button(RowAction::Disable)->click();
        page.reset();
        daemon.hold = false;
        daemon.answer();
        QCOMPARE(daemon.count("runner.list"), 2);
    }

    void aFailedCreateSaysWhy() {
        FakeDaemon daemon;
        daemon.answers["runner.pairing.create"] = {
            {{"ok", false}, {"error_code", "too_many_pairings"}}, "rejected"};
        SettingsRunnersPage page(daemon.requester());
        page.show();
        auto* pairing = startPairing(page);
        QVERIFY(pairing->error().startsWith("Too many pairings"));
        QVERIFY(pairing->isFinished());
        QTest::keyClick(pairing->dialog(), Qt::Key_Escape);
        QCOMPARE(daemon.count("runner.pairing.reject"), 0);
    }

    void everyControlCanBeReachedAndNamed() {
        FakeDaemon daemon;
        SettingsRunnersPage page(daemon.requester());
        page.resize(900, 700);
        page.show();
        for (ket::Button* button : page.findChildren<ket::Button*>()) {
            if (!button->isVisible())
                continue;
            QVERIFY2(button->focusPolicy() & Qt::TabFocus, qPrintable(button->text()));
            QVERIFY2(!button->accessibleName().isEmpty() || !button->text().isEmpty(),
                     qPrintable(button->objectName()));
        }
        QVERIFY(page.row("r1")->button(RowAction::Remove)->accessibleName().contains("alpha"));
        auto* pairing = startPairing(page);
        QVERIFY(!pairing->tokenField()->accessibleName().isEmpty());
        // Space on a focused button is a deliberate press; Tab reaches the copy buttons.
        pairing->copyCommandButton()->setFocus();
        QTest::keyClick(pairing->copyCommandButton(), Qt::Key_Space);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("ketplus-runner register -"));
        QGuiApplication::clipboard()->clear();
    }

    void themeAndZoomChangeWhileOpen_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void themeAndZoomChangeWhileOpen() {
        QFETCH(bool, dark);
        ThemeManager theme(*qApp);
        theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
        ket::Appearance& appearance = ket::Appearance::instance();
        const qreal zoom = appearance.zoom();
        const auto restore = qScopeGuard([&] { appearance.setZoom(zoom); });
        FakeDaemon daemon;
        auto* dialog = new SettingsDialog(AppearanceSettings{}, theme);
        SettingsWorkspacePage shell(dialog, {}, nullptr, {}, {}, {}, daemon.requester());
        shell.resize(1200, 800);
        shell.show();
        shell.showSection("runners");
        auto* page = shell.runnersPage();
        QVERIFY(page);
        QTRY_VERIFY(page->isVisible());
        QTRY_COMPARE(page->rows().size(), 3);
        const QString capture = qEnvironmentVariable("KETPLUS_RUNNERS_CAPTURE");
        const QString name = dark ? QStringLiteral("dark") : QStringLiteral("light");
        const auto save = [&](const QString& state) {
            if (capture.isEmpty())
                return;
            QTest::qWait(300); // layout and the section scroll settle before the capture
            QVERIFY(shell.grab().save(capture + "/runners-" + state + "-" + name + ".png"));
        };
        save("list");
        auto* first = page->rows().first();
        theme.setMode(dark ? ThemeManager::Mode::Light : ThemeManager::Mode::Dark);
        theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
        for (const qreal step : {1.25, 1.5, 1.0}) {
            appearance.setZoom(step);
            QCOMPARE(shell.runnersPage(), page);
            QCOMPARE(page->rows().first(), first);
        }
        auto* pairing = startPairing(*page);
        save("add");
        pairing->checkButton()->click();
        QVERIFY(pairing->dialog()->findChild<QLabel*>("runnerPairingCode")->isVisible());
        appearance.setZoom(1.25);
        appearance.setZoom(1.0);
        save("confirm");
        QTest::keyClick(pairing->dialog(), Qt::Key_Escape);
        QTRY_VERIFY(!page->pairingDialog());

        daemon.answers["runner.list"].first["runners"] = QJsonArray{};
        page->refresh();
        save("empty");
        daemon.answers["runner.list"] = {{{"ok", false}, {"error_code", "rate_limited"}},
                                         "rejected"};
        page->refresh();
        QVERIFY(!page->error().isEmpty());
        save("error");
    }
};

QTEST_MAIN(SettingsRunnersPageTest)
#include "SettingsRunnersPageTest.moc"
