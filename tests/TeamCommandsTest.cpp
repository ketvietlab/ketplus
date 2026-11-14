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
        return page.findChild<QFrame*>("teamDecision_D-1")->findChild<QLabel*>("teamDecisionError");
    }

  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ket::Style::install(*qApp);
    }

    // Criteria 1 and 2: in the click's own pass the choice looks pressed and every choice of
    // the decision is disabled; clicking, Space or Enter again sends nothing.
    void aChoiceIsPendingAtOnceAndSentOnce() {
        start(fixture());
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(900, 700);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        daemon_.hold = true;
        const auto both = choices(page);
        QCOMPARE(both.size(), 2);
        ket::Button* stripe = both.at(0);
        ket::Button* paypal = both.at(1);
        QCOMPARE(stripe->property("choice").toString(), QStringLiteral("Stripe"));
        stripe->setFocus();
        QTest::keyClick(stripe, Qt::Key_Space);
        // No event has been processed since the key: the state is already drawn.
        QVERIFY(pressed(stripe));
        QVERIFY(!stripe->isEnabled());
        QVERIFY(!paypal->isEnabled());
        QVERIFY(!pressed(paypal));
        QCOMPARE(stripe->accessibleDescription(), QStringLiteral("Sending…"));
        QCOMPARE(daemon_.count("team.decision.answer"), 1);
        QVERIFY(daemon_.late.size() == 1);

        stripe->click();
        paypal->click();
        QTest::mouseClick(stripe, Qt::LeftButton);
        QTest::keyClick(stripe, Qt::Key_Space);
        QTest::keyClick(paypal, Qt::Key_Return);
        QTest::keyClick(paypal, Qt::Key_Enter);
        QVERIFY(!teams_->commands()->send(TeamCommands::key("7", "decision/D-1"), "PayPal",
                                          "team.decision.answer", {}));
        QCOMPARE(daemon_.count("team.decision.answer"), 1);
        QCOMPARE(daemon_.sent.last().second.value("choice").toString(), QStringLiteral("Stripe"));
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
        bar->setValue(60);
        daemon_.hold = true;
        daemon_.answers.insert("team.decision.answer",
                               {{"ok", true}, {"team", fixture(2, false, false)}});
        ket::Button* paypal = choices(page).at(1);
        paypal->setFocus();
        QTest::keyClick(paypal, Qt::Key_Space);
        QVERIFY(pressed(paypal));
        QVERIFY(teams_->commands()->isPending(TeamCommands::key("7", "decision/D-1")));

        daemon_.deliver();
        QVERIFY(!teams_->commands()->isPending(TeamCommands::key("7", "decision/D-1")));
        QTRY_VERIFY(!page.findChild<QFrame*>("teamDecision_D-1"));
        QTRY_VERIFY(!page.findChild<QLabel*>("teamWaitingHeading"));
        QApplication::processEvents();
        QCOMPARE(bar->value(), 60);
        QWidget* focused = page.window()->focusWidget();
        QVERIFY(focused);
        QVERIFY(page.isAncestorOf(focused));
        QCOMPARE(daemon_.count("team.decision.answer"), 1);
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
            daemon_.answers.insert("team.decision.answer",
                                   {{"ok", false}, {"error_message", reason}});
            ket::Button* stripe = choices(page).at(0);
            stripe->setFocus();
            QTest::keyClick(stripe, Qt::Key_Space);
            QVERIFY(!stripe->isEnabled());
            daemon_.deliver();
            for (ket::Button* choice : choices(page)) {
                QVERIFY(choice->isEnabled());
                QVERIFY(!pressed(choice));
                QVERIFY(choice->accessibleDescription().isEmpty());
            }
            QLabel* status = decisionStatus(page);
            QCOMPARE(status->text(), reason);
            QVERIFY(status->isVisibleTo(&page));
            // The focus is back on the choice the keyboard was on, and it answers again.
            QCOMPARE(page.window()->focusWidget(), stripe);
            daemon_.hold = false;
            QTest::keyClick(stripe, Qt::Key_Space);
            QCOMPARE(daemon_.count("team.decision.answer"), 2);
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
        daemon_.answers.insert("team.decision.answer",
                               {{"ok", false}, {"error_message", "Try again."}});
        const QPointer<ket::Button> before = choices(page).at(1);
        before->click();
        QJsonObject next = fixture(5);
        next.insert("name", "Shop 2");
        teams_->applyEvent("team.changed", {{"team", next}});
        QVERIFY(!before); // the content was rebuilt
        const auto rebuilt = choices(page);
        QCOMPARE(rebuilt.at(1)->property("choice").toString(), QStringLiteral("PayPal"));
        QVERIFY(pressed(rebuilt.at(1)));
        QVERIFY(!rebuilt.at(1)->isEnabled());
        QVERIFY(!rebuilt.at(0)->isEnabled());
        QVERIFY(!pressed(rebuilt.at(0)));
        rebuilt.at(0)->click();
        QCOMPARE(daemon_.count("team.decision.answer"), 1);
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
        daemon_.answers.insert("team.decision.answer", {{"ok", false}, {"error_message", "No."}});
        auto* resume = page.findChild<ket::Banner*>("teamPausedBanner")->action();
        QCOMPARE(resume->text(), QStringLiteral("Resume"));
        resume->click();
        QVERIFY(pressed(resume) && !resume->isEnabled());
        QTRY_COMPARE(resume->text(), QStringLiteral("Still waiting…"));
        QVERIFY(!resume->isEnabled());
        QVERIFY(teams_->commands()->isWaitingLong(TeamCommands::key("7", "paused")));
        // A decision says it in the label beside its choices.
        ket::Button* stripe = choices(page).at(0);
        stripe->click();
        QLabel* status = decisionStatus(page);
        QTRY_VERIFY(status->isVisibleTo(&page));
        QVERIFY(status->text().startsWith("Still waiting"));
        QCOMPARE(stripe->text(), QStringLiteral("Stripe (recommended)"));

        // The pause in the page's menu waits for the same command.
        QAction pause("Pause automatic turns");
        teams_->commands()->bind(&pause, TeamCommands::key("7", "paused"));
        QVERIFY(!pause.isEnabled());
        QVERIFY(pause.text().endsWith("(still waiting)"));

        daemon_.deliver();
        QTRY_VERIFY(!page.findChild<ket::Banner*>("teamPausedBanner"));
        QVERIFY(pause.isEnabled());
        QCOMPARE(pause.text(), QStringLiteral("Pause automatic turns"));
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
