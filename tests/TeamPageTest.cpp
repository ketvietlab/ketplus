// #466: the team page against a fake daemon — each section from a fixture team, the empty and
// paused states, answering, the task dialog, the handover request and a live update.
// Run with QT_QPA_PLATFORM=offscreen.
#include "teams/TeamPage.h"
#include "KetBanner.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetEmptyState.h"
#include "KetSegmentedControl.h"
#include "KetSelect.h"
#include "KetStyle.h"
#include "KetTextField.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamTaskDialog.h"

#include <QApplication>
#include <QJsonArray>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTableWidget>
#include <QtTest>

using namespace ketplus;

namespace {

QJsonObject member(const QString& session, const QString& role, const QString& name,
                   const QString& lane = {}) {
    QJsonObject json{{"session_id", session}, {"role", role}, {"name", name}, {"number", "1"}};
    if (role == "developer")
        json.insert("lane", lane), json.insert("paths", QJsonArray{"src/" + lane.toLower()});
    return json;
}

QJsonObject task(const QString& id, const QString& state, const QString& owner,
                 const QJsonArray& outside = {}) {
    return {{"id", id},
            {"title", "Task " + id},
            {"state", state},
            {"owner", owner},
            {"lane", "Web"},
            {"criteria", QJsonArray{QJsonObject{{"text", "Works"}, {"met", true}},
                                    QJsonObject{{"text", "Tested"}, {"met", false}}}},
            {"criteria_met", 1},
            {"outside_lane", outside},
            {"depends_on", QJsonArray{}},
            {"history", QJsonArray{QJsonObject{{"at", "2026-11-10T10:00:00.000Z"},
                                               {"actor", "lead"},
                                               {"kind", "created"},
                                               {"text", "Created"}},
                                   QJsonObject{{"at", "2026-11-10T10:05:00.000Z"},
                                               {"actor", "lead"},
                                               {"kind", "criteria"},
                                               {"text", "Criteria changed"}},
                                   QJsonObject{{"at", "2026-11-10T10:06:00.000Z"},
                                               {"actor", "user"},
                                               {"kind", "lane"},
                                               {"text", "Lane changed to Web"}}}}};
}

QJsonObject fixture(int revision = 1, int developers = 2, bool paused = false,
                    bool decision = true) {
    QJsonArray members{member("lead", "leader", "Lead")};
    for (int index = 1; index <= developers; ++index)
        members.append(member(QStringLiteral("dev%1").arg(index), "developer",
                              QStringLiteral("Dev %1").arg(index),
                              index == 1 ? "Web" : QStringLiteral("Api%1").arg(index)));
    members.append(member("rev", "reviewer", "Review"));
    QJsonArray decisions;
    if (decision)
        decisions.append(QJsonObject{{"id", "D-1"},
                                     {"kind", "question"},
                                     {"question", "Use Stripe?"},
                                     {"context", "Payments need a provider."},
                                     {"choices", QJsonArray{"PayPal", "Stripe"}},
                                     {"recommended", "Stripe"},
                                     {"task_id", "T-2"},
                                     {"raised_by", "lead"},
                                     {"raised_at", "2026-11-10T10:00:00.000Z"},
                                     {"state", "open"}});
    return {{"id", "7"},
            {"name", "Shop"},
            {"goal", "Checkout in one page"},
            {"repository", "/work/shop"},
            {"integration_branch", "team/shop"},
            {"merge_policy", "leader"},
            {"paused", paused},
            {"paused_reason", paused ? "budget" : ""},
            {"members", members},
            {"tasks", QJsonArray{task("T-1", "in_progress", "dev1", QJsonArray{"docs/x.md"}),
                                 task("T-2", "blocked", "dev2"), task("T-3", "ready", "dev1"),
                                 task("T-4", "done", "dev2")}},
            {"decisions", decisions},
            {"open_decisions", decision ? 1 : 0},
            {"revision", revision}};
}

struct FakeDaemon {
    QList<QPair<QString, QJsonObject>> sent;
    QHash<QString, QJsonObject> answers;
    TeamDirectory::Transport transport() {
        return [this](const QString& type, const QJsonObject& payload, TeamDirectory::Reply reply) {
            sent.append({type, payload});
            const QJsonObject answer = answers.value(type, {{"ok", true}});
            reply(answer, answer.value("ok").toBool() ? QString{}
                                                      : answer.value("error_message").toString());
        };
    }
    int count(const QString& type) const {
        return static_cast<int>(std::count_if(
            sent.cbegin(), sent.cend(), [&](const auto& entry) { return entry.first == type; }));
    }
    QJsonObject last(const QString& type) const {
        for (auto it = sent.crbegin(); it != sent.crend(); ++it)
            if (it->first == type)
                return it->second;
        return {};
    }
};

QStringList labels(QWidget* root, const char* name) {
    QStringList result;
    for (auto* label : root->findChildren<QLabel*>(QLatin1String(name)))
        result.append(label->text());
    return result;
}

} // namespace

class TeamPageTest final : public QObject {
    Q_OBJECT

  private:
    FakeDaemon daemon_;
    std::unique_ptr<TeamDirectory> teams_;

    void start(const QJsonObject& team) {
        daemon_ = {};
        daemon_.answers.insert("team.list", {{"ok", true}, {"teams", QJsonArray{team}}});
        daemon_.answers.insert(
            "team.get", {{"ok", true},
                         {"team", team},
                         {"updates", QJsonArray{QJsonObject{{"id", 1},
                                                            {"at", "2026-11-10T10:00:00.000Z"},
                                                            {"text", "T-1 created"}},
                                                QJsonObject{{"id", 2},
                                                            {"at", "2026-11-10T10:01:00.000Z"},
                                                            {"text", "T-1 assigned"}}}}});
        teams_ = std::make_unique<TeamDirectory>();
        teams_->setTransport(daemon_.transport());
        teams_->setAvailable(true);
    }

  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ket::Style::install(*qApp);
    }

    void everySectionShowsFromTheTeam() {
        start(fixture());
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1024, 640);
        page.show();
        QTRY_COMPARE(page.updates().size(), 2);
        QCOMPARE(page.findChild<QLabel*>("teamPageName")->text(), QStringLiteral("Shop"));
        QCOMPARE(page.findChild<QLabel*>("teamPageGoal")->text(),
                 QStringLiteral("Checkout in one page"));
        const QString facts = page.findChild<QLabel*>("teamPageFacts")->text();
        QVERIFY2(facts.contains("into team/shop") && facts.contains("the leader merges tasks") &&
                     facts.contains("1 of 4 tasks done"),
                 qPrintable(facts));
        // Waiting for you: the recommended choice first and marked.
        auto* card = page.findChild<QFrame*>("teamDecision_D-1");
        QVERIFY(card);
        const auto choices = card->findChildren<ket::Button*>("teamDecisionChoice");
        QCOMPARE(choices.size(), 2);
        QCOMPARE(choices.first()->property("choice").toString(), QStringLiteral("Stripe"));
        QVERIFY(choices.first()->text().contains("recommended"));
        QVERIFY(card->findChild<QLabel*>("teamDecisionAbout")->text().contains("blocks T-2"));
        // Tasks, with the lane flag and Ready to merge.
        auto* table = page.tasksTable();
        QCOMPARE(table->rowCount(), 4);
        QCOMPARE(table->item(0, 2)->text(), QStringLiteral("1/2"));
        QVERIFY(table->item(0, 5)->text().contains("Outside its lane"));
        QCOMPARE(table->item(2, 5)->text(), QStringLiteral("Ready to merge"));
        page.setTaskFilter("blocked");
        QCOMPARE(page.tasksTable()->rowCount(), 1);
        QCOMPARE(page.tasksTable()->item(0, 0)->text(), QStringLiteral("T-2"));
        // Members: one row each, what each does; the developer's menu removes it.
        QVERIFY(page.findChild<QWidget*>("teamMember_lead"));
        QVERIFY(page.findChild<QWidget*>("teamMember_dev1")
                    ->findChild<QLabel*>("teamMemberFacts")
                    ->text()
                    .contains("On T-1"));
        QVERIFY(page.findChild<QWidget*>("teamMember_dev1")
                    ->findChild<ket::Button*>("teamMemberRemove")
                    ->isEnabled());
        QVERIFY(!page.findChild<QWidget*>("teamMember_rev")
                     ->findChild<ket::Button*>("teamMemberRemove"));
        QSignalSpy opened(&page, &TeamPage::conversationRequested);
        page.findChild<QWidget*>("teamMember_dev2")
            ->findChild<ket::Button*>("teamMemberOpen")
            ->click();
        QCOMPARE(opened.first().first().toString(), QStringLiteral("dev2"));
        // Updates, newest first.
        const QStringList updates = labels(&page, "teamUpdate");
        QCOMPARE(updates.size(), 2);
        QVERIFY(updates.first().endsWith("T-1 assigned"));
        QVERIFY(!page.findChild<ket::Banner*>("teamPausedBanner"));
        QVERIFY(!page.findChild<ket::Banner*>("teamQueueBanner"));
    }

    void pausedAndQueueBannersAndTheEmptyPage() {
        start(fixture(1, 4, true, false));
        TeamPage page(teams_.get(), "7", "/work/shop");
        auto* paused = page.findChild<ket::Banner*>("teamPausedBanner");
        QVERIFY(paused);
        QVERIFY(paused->message().contains("automatic turns"));
        paused->action()->click();
        QCOMPARE(daemon_.last("team.update").value("paused").toBool(true), false);
        QVERIFY(page.findChild<ket::Banner*>("teamQueueBanner"));
        QVERIFY(!page.findChild<QLabel*>("teamWaitingHeading")); // nothing waits
        TeamPage empty(teams_.get(), "99", "/work/other");
        auto* state = empty.findChild<ket::EmptyState*>("teamPageEmpty");
        QVERIFY(state);
        QSignalSpy create(&empty, &TeamPage::newTeamRequested);
        state->action()->click();
        QCOMPARE(create.first().first().toString(), QStringLiteral("/work/other"));
    }

    // Answering sends team.decision.answer; the answer's team removes the card.
    void answeringRemovesTheDecision() {
        start(fixture());
        TeamPage page(teams_.get(), "7", "/work/shop");
        daemon_.answers.insert("team.decision.answer",
                               {{"ok", true}, {"team", fixture(2, 2, false, false)}});
        page.findChild<QFrame*>("teamDecision_D-1")
            ->findChildren<ket::Button*>("teamDecisionChoice")
            .first()
            ->click();
        const QJsonObject sent = daemon_.last("team.decision.answer");
        QCOMPARE(sent.value("team_id").toString(), QStringLiteral("7"));
        QCOMPARE(sent.value("decision_id").toString(), QStringLiteral("D-1"));
        QCOMPARE(sent.value("choice").toString(), QStringLiteral("Stripe"));
        QTRY_VERIFY(!page.findChild<QFrame*>("teamDecision_D-1"));
        QCOMPARE(teams_->team("7")->value("open_decisions").toInt(), 0);
    }

    void theTaskDialogShowsHistoryAndHandsOver() {
        start(fixture());
        QWidget window;
        window.resize(1024, 640);
        TeamPage page(teams_.get(), "7", "/work/shop", &window);
        window.show();
        auto* dialog = page.openTask("T-1");
        auto* body = dialog->findChild<TeamTaskDialog*>();
        QVERIFY(body);
        const QStringList history = labels(dialog, "teamTaskHistory");
        QCOMPARE(history.size(), 3);
        QVERIFY(history.at(1).contains("Criteria changed"));
        QVERIFY(history.at(2).contains("Lane changed"));
        QCOMPARE(labels(dialog, "teamTaskCriterion").size(), 2);
        // Only another developer can take it; a reason is required.
        QCOMPARE(body->handoverOwner()->count(), 1);
        QCOMPARE(body->handoverOwner()->currentData().toString(), QStringLiteral("dev2"));
        dialog->findChild<ket::Button*>("teamHandoverButton")->click();
        QCOMPARE(daemon_.count("team.task.handover"), 0);
        QVERIFY(!body->error().isEmpty());
        body->handoverReason()->setText("Dev 1 is on T-3");
        QJsonObject after = fixture(3);
        QJsonArray tasks = after.value("tasks").toArray();
        QJsonObject moved = tasks.at(0).toObject();
        moved.insert("owner", "dev2");
        tasks.replace(0, moved);
        after.insert("tasks", tasks);
        daemon_.answers.insert("team.task.handover", {{"ok", true}, {"team", after}});
        dialog->findChild<ket::Button*>("teamHandoverButton")->click();
        const QJsonObject sent = daemon_.last("team.task.handover");
        QCOMPARE(sent.value("task_id").toString(), QStringLiteral("T-1"));
        QCOMPARE(sent.value("owner").toString(), QStringLiteral("dev2"));
        QCOMPARE(sent.value("reason").toString(), QStringLiteral("Dev 1 is on T-3"));
        QVERIFY(body->error().isEmpty());
        // The row and the open dialog show the new owner.
        QTRY_VERIFY(page.tasksTable()->item(0, 3)->text().startsWith("Dev 2"));
        QVERIFY(dialog->isOpen());
        QVERIFY(dialog->findChild<QLabel*>("teamTaskFacts")->text().contains("Dev 2"));
        dialog->reject();
    }

    // A team.changed from another client updates the page in place of where the reader was.
    void aLiveUpdateKeepsTheScrollPosition() {
        start(fixture());
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(700, 300);
        page.show();
        QApplication::processEvents();
        auto* bar = page.scrollArea()->verticalScrollBar();
        QVERIFY(bar->maximum() > 40);
        bar->setValue(40);
        QJsonObject next = fixture(5);
        next.insert("name", "Shop 2");
        teams_->applyEvent("team.changed", {{"team", next}});
        QCOMPARE(page.findChild<QLabel*>("teamPageName")->text(), QStringLiteral("Shop 2"));
        QApplication::processEvents();
        QCOMPARE(bar->value(), 40);
    }
};

QTEST_MAIN(TeamPageTest)
#include "TeamPageTest.moc"
