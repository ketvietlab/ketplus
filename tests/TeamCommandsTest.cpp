// A team command shows at once that it was taken and is sent once (TeamCommands): the team
// page's decision choices, its banner and menu, and its dialogs, against a daemon that holds
// its answers until the test delivers them.

#include "teams/TeamCommands.h"
#include "KetBanner.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetStyle.h"
#include "KetTextField.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPage.h"
#include "teams/TeamTurnBudget.h"

#include <QAction>
#include <QApplication>
#include <QJsonArray>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QtTest>

#include <functional>
#include <memory>

using namespace ketplus;

namespace {

QJsonObject member(const QString& session, const QString& role, const QString& name) {
    return {{"session_id", session}, {"role", role}, {"name", name}, {"state", "idle"}};
}

QJsonObject fixture(int revision = 1, bool paused = false, bool decision = true) {
    QJsonArray decisions;
    if (decision)
        decisions.append(QJsonObject{{"id", "D-1"},
                                     {"kind", "question"},
                                     {"question", "Use Stripe?"},
                                     {"choices", QJsonArray{"PayPal", "Stripe"}},
                                     {"recommended", "Stripe"},
                                     {"raised_by", "lead"},
                                     {"raised_at", "2026-11-10T10:00:00.000Z"},
                                     {"state", "open"}});
    QJsonArray tasks;
    for (int index = 1; index <= 12; ++index)
        tasks.append(QJsonObject{{"id", QStringLiteral("T-%1").arg(index)},
                                 {"title", QStringLiteral("Task %1").arg(index)},
                                 {"state", "ready"},
                                 {"owner", "dev1"}});
    return {{"id", "7"},
            {"name", "Shop"},
            {"repository", "/work/shop"},
            {"merge_policy", "leader"},
            {"paused", paused},
            {"paused_reason", paused ? "user" : ""},
            {"turn_budget", 60},
            {"members",
             QJsonArray{member("lead", "leader", "Lead"), member("dev1", "developer", "Dev 1"),
                        member("rev", "reviewer", "Review")}},
            {"tasks", tasks},
            {"decisions", decisions},
            {"open_decisions", decision ? 1 : 0},
            {"revision", revision}};
}

// Every answer waits in `late` until the test delivers it, as a busy daemon would.
struct SlowDaemon {
    QList<QPair<QString, QJsonObject>> sent;
    QHash<QString, QJsonObject> answers;
    bool hold{false};
    QList<std::function<void()>> late;
    TeamDirectory::Transport transport() {
        return [this](const QString& type, const QJsonObject& payload, TeamDirectory::Reply reply) {
            sent.append({type, payload});
            const QJsonObject answer = answers.value(type, {{"ok", true}});
            auto deliver = [answer, reply] {
                reply(answer, answer.value("ok").toBool()
                                  ? QString{}
                                  : answer.value("error_message").toString());
            };
            if (hold)
                late.append(deliver);
            else
                deliver();
        };
    }
    int count(const QString& type) const {
        return static_cast<int>(std::count_if(
            sent.cbegin(), sent.cend(), [&](const auto& entry) { return entry.first == type; }));
    }
    void deliver() {
        const auto waiting = std::exchange(late, {});
        for (const auto& answer : waiting)
            answer();
    }
};

bool pressed(const QAbstractButton* control) {
    return control->property("teamCommandPending").toBool();
}

} // namespace

class TeamCommandsTest final : public QObject {
    Q_OBJECT

    SlowDaemon daemon_;
    std::unique_ptr<TeamDirectory> teams_;

    void start(const QJsonObject& team) {
        QSettings().remove(QStringLiteral("teams/decisionAnswers/7"));
        daemon_ = {};
        daemon_.answers.insert("team.list", {{"ok", true}, {"teams", QJsonArray{team}}});
        daemon_.answers.insert("team.get", {{"ok", true}, {"team", team}});
        teams_ = std::make_unique<TeamDirectory>();
        teams_->setTransport(daemon_.transport());
        teams_->setAvailable(true);
    }
    // The decision's choices, in the page's order: Stripe (recommended), then PayPal.
    static QList<ket::Button*> choices(TeamPage& page) {
        return page.findChild<QFrame*>("teamDecision_D-1")
            ->findChildren<ket::Button*>("teamDecisionChoice");
    }
    static QLabel* decisionStatus(TeamPage& page) {
        return page.findChild<QLabel*>("teamDecisionError");
    }

  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ket::Style::install(*qApp);
    }

    void aSubmitIsPendingAtOnceAndSentOnce() {
        start(fixture());
        TeamPage page(teams_.get(), "7", "/work/shop");
        daemon_.hold = true;
        choices(page).first()->click();
        QCOMPARE(daemon_.count("team.decisions.answer_batch"), 0);
        auto* submit = page.findChild<ket::Button*>("teamDecisionSubmitAll");
        submit->click();
        QVERIFY(!submit->isEnabled());
        QVERIFY(teams_->commands()->isPending(TeamCommands::key("7", "decision-batch")));
        QVERIFY(!choices(page).first()->isEnabled());
        submit->click();
        QCOMPARE(daemon_.count("team.decisions.answer_batch"), 1);
        const auto sent = daemon_.sent.last().second.value("answers").toArray();
        QCOMPARE(sent.first().toObject().value("answer").toString(), "Stripe");
    }

    // Criterion 3: the answer's team ends the pending state; the decision leaves Waiting for
    // you, and the scroll position and the focus stay in the page.
    void theAnswerEndsItWithTheConfirmedState() {
        start(fixture());
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(700, 320);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        QApplication::processEvents();
        auto* bar = page.scrollArea()->verticalScrollBar();
        QVERIFY(bar->maximum() > 60);
        daemon_.hold = true;
        daemon_.answers.insert("team.decisions.answer_batch",
                               {{"ok", true}, {"team", fixture(2, false, false)}});
        ket::Button* paypal = choices(page).at(1);
        paypal->setFocus();
        QTest::keyClick(paypal, Qt::Key_Space);
        page.findChild<ket::Button*>("teamDecisionSubmitAll")->click();
        QVERIFY(teams_->commands()->isPending(TeamCommands::key("7", "decision-batch")));

        bar->setValue(60);
        daemon_.deliver();
        QVERIFY(!teams_->commands()->isPending(TeamCommands::key("7", "decision-batch")));
        QTRY_VERIFY(!page.findChild<QFrame*>("teamDecision_D-1"));
        QTRY_VERIFY(!page.findChild<QLabel*>("teamWaitingHeading"));
        QApplication::processEvents();
        QCOMPARE(bar->value(), 60);
        QWidget* focused = page.window()->focusWidget();
        QVERIFY(focused);
        QVERIFY(page.isAncestorOf(focused));
        QCOMPARE(daemon_.count("team.decisions.answer_batch"), 1);
    }

    // Criterion 4: a refusal or a lost daemon gives every choice back, the reason beside them.
    void aRefusalGivesTheChoicesBack() {
        for (const bool transport : {false, true}) {
            start(fixture());
            TeamPage page(teams_.get(), "7", "/work/shop");
            page.resize(900, 700);
            page.show();
            QVERIFY(QTest::qWaitForWindowExposed(&page));
            daemon_.hold = true;
            const QString reason = transport ? QStringLiteral("KetPlus Daemon is not reachable.")
                                             : QStringLiteral("The decision is already answered.");
            daemon_.answers.insert("team.decisions.answer_batch",
                                   {{"ok", false}, {"error_message", reason}});
            ket::Button* stripe = choices(page).at(0);
            stripe->setFocus();
            QTest::keyClick(stripe, Qt::Key_Space);
            page.findChild<ket::Button*>("teamDecisionSubmitAll")->click();
            QVERIFY(!stripe->isEnabled());
            daemon_.deliver();
            for (ket::Button* choice : choices(page)) {
                QVERIFY(choice->isEnabled());
                QVERIFY(choice->accessibleDescription().isEmpty());
            }
            QLabel* status = decisionStatus(page);
            QCOMPARE(status->text(), reason);
            QVERIFY(status->isVisibleTo(&page));
            // The focus is back on the choice the keyboard was on, and it answers again.
            daemon_.hold = false;
            page.findChild<ket::Button*>("teamDecisionSubmitAll")->click();
            QCOMPARE(daemon_.count("team.decisions.answer_batch"), 2);
        }
    }

    // Criterion 5: another change of the team rebuilds the page; the new choices are still
    // pending, and the answer then ends them.
    void aRebuildKeepsThePendingAnswer() {
        start(fixture());
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(900, 700);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        daemon_.hold = true;
        daemon_.answers.insert("team.decisions.answer_batch",
                               {{"ok", false}, {"error_message", "Try again."}});
        const QPointer<ket::Button> before = choices(page).at(1);
        before->click();
        page.findChild<ket::Button*>("teamDecisionSubmitAll")->click();
        QJsonObject next = fixture(5);
        next.insert("name", "Shop 2");
        // T-6: the decision's card is built again only when its decision changed, once the
        // event loop has handled the change.
        QJsonArray decisions = next.value("decisions").toArray();
        QJsonObject decision = decisions.at(0).toObject();
        decision.insert("context", "Asked again.");
        decisions.replace(0, decision);
        next.insert("decisions", decisions);
        teams_->applyEvent("team.changed", {{"team", next}});
        QCoreApplication::processEvents();
        QVERIFY(!before); // the card was rebuilt
        const auto rebuilt = choices(page);
        QCOMPARE(rebuilt.at(1)->property("choice").toString(), QStringLiteral("PayPal"));
        QVERIFY(rebuilt.at(1)->isChecked());
        QVERIFY(!rebuilt.at(1)->isEnabled());
        QVERIFY(!rebuilt.at(0)->isEnabled());
        QVERIFY(!rebuilt.at(0)->isChecked());
        rebuilt.at(0)->click();
        QCOMPARE(daemon_.count("team.decisions.answer_batch"), 1);
        daemon_.deliver();
        QVERIFY(rebuilt.at(0)->isEnabled() && rebuilt.at(1)->isEnabled());
        QCOMPARE(decisionStatus(page)->text(), QStringLiteral("Try again."));
    }

    // The leader's choice: after the limit the control says it is still waiting and stays
    // pending; the answer still ends it.
    void aSlowAnswerSaysItIsStillWaiting() {
        start(fixture(1, true, true));
        teams_->commands()->setStillWaitingForTesting(50);
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(900, 700);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        daemon_.hold = true;
        // The answers are chosen when the requests are sent, delivered at the end.
        daemon_.answers.insert("team.update", {{"ok", true}, {"team", fixture(2, false, true)}});
        daemon_.answers.insert("team.decisions.answer_batch",
                               {{"ok", false}, {"error_message", "No."}});
        auto* banner = page.findChild<ket::Banner*>("teamPausedBanner");
        QVERIFY(banner);
        auto* resume = banner->action();
        QVERIFY(resume);
        QCOMPARE(resume->text(), QStringLiteral("Resume"));
        resume->click();
        QVERIFY(pressed(resume) && !resume->isEnabled());
        QTRY_COMPARE(resume->text(), QStringLiteral("Still waiting…"));
        QVERIFY(!resume->isEnabled());
        QVERIFY(teams_->commands()->isWaitingLong(TeamCommands::key("7", "paused")));
        // A decision says it in the label beside its choices.
        QVERIFY(page.findChild<QFrame*>("teamDecision_D-1"));
        QVERIFY(!choices(page).isEmpty());
        ket::Button* stripe = choices(page).at(0);
        stripe->click();
        auto* submit = page.findChild<ket::Button*>("teamDecisionSubmitAll");
        QVERIFY(submit);
        QVERIFY(submit->isEnabled());
        submit->click();
        QTRY_VERIFY(decisionStatus(page) && decisionStatus(page)->isVisibleTo(&page));
        QVERIFY(decisionStatus(page)->text().startsWith("Still waiting"));
        QCOMPARE(choices(page).first()->text(), QStringLiteral("Stripe (recommended)"));

        // The pause in the page's menu waits for the same command.
        QAction pause("Pause automatic turns");
        teams_->commands()->bind(&pause, TeamCommands::key("7", "paused"));
        QVERIFY(!pause.isEnabled());
        QVERIFY(pause.text().endsWith("(still waiting)"));

        daemon_.deliver();
        QTRY_VERIFY(!page.findChild<ket::Banner*>("teamPausedBanner"));
        QVERIFY(pause.isEnabled());
        QCOMPARE(pause.text(), QStringLiteral("Pause automatic turns"));
        QTRY_VERIFY(decisionStatus(page));
        QCOMPARE(decisionStatus(page)->text(), QStringLiteral("No."));
        QCOMPARE(daemon_.count("team.update"), 1);
    }

    // Criterion 7 in a dialog: Save of the automatic turns waits for the daemon, sent once.
    void aDialogsActionIsSentOnce() {
        start(fixture());
        QWidget window;
        window.resize(900, 700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        daemon_.hold = true;
        QPointer<ket::Dialog> dialog =
            TeamTurnBudgetForm::present(&window, teams_.get(), *teams_->team("7"));
        auto* form = dialog->findChild<TeamTurnBudgetForm*>();
        form->budgetField()->setText("80");
        auto* save = dialog->findChild<ket::Button*>("teamTurnBudgetSave");
        save->click();
        QVERIFY(pressed(save) && !save->isEnabled());
        save->click();
        QTest::keyClick(form->budgetField(), Qt::Key_Return);
        QCOMPARE(daemon_.count("team.update"), 1);
        daemon_.deliver();
        QTRY_VERIFY(!dialog || !dialog->isOpen());
    }
};

QTEST_MAIN(TeamCommandsTest)
#include "TeamCommandsTest.moc"
