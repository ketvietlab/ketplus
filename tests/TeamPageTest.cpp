// #466: the team page against a fake daemon — each section from a fixture team, the empty and
// paused states, answering, the task dialog, the handover request and a live update.
// Run with QT_QPA_PLATFORM=offscreen.
#include "teams/TeamPage.h"
#include "KetBanner.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetEmptyState.h"
#include "KetExpandableText.h"
#include "KetSegmentedControl.h"
#include "KetSelect.h"
#include "KetStyle.h"
#include "KetTextArea.h"
#include "KetTextField.h"
#include "teams/TeamDialogs.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamGoal.h"
#include "teams/TeamPageParts.h"
#include "teams/TeamTaskDialog.h"
#include "teams/TeamTurnBudget.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QHeaderView>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTextEdit>
#include <QtTest>
#include <functional>

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
            {"turn_budget", 60},
            {"automatic_turns", 23},
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
    // While set, answers wait in `late` until the test delivers them.
    bool hold{false};
    QList<std::function<void()>> late;
    TeamDirectory::Transport transport() {
        return [this](const QString& type, const QJsonObject& payload, TeamDirectory::Reply reply) {
            sent.append({type, payload});
            const QJsonObject answer = answers.value(type, {{"ok", true}});
            const auto deliver = [answer, reply] {
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
        QCOMPARE(page.findChild<ket::ExpandableText*>("teamPageGoal")->text(),
                 QStringLiteral("Checkout in one page"));
        QCOMPARE(page.findChild<QLabel*>("teamPageRepository")->text(), QStringLiteral("shop"));
        QCOMPARE(page.findChild<QLabel*>("teamPageBranch")->text(), QStringLiteral("team/shop"));
        QCOMPARE(page.findChild<QLabel*>("teamPageMerges")->text(),
                 QStringLiteral("The leader merges tasks"));
        QCOMPARE(page.findChild<QLabel*>("teamPageProgress")->text(),
                 QStringLiteral("1 of 4 tasks done"));
        // ADR 0114, open point 2: the turns used of the team's own budget.
        QCOMPARE(page.findChild<QLabel*>("teamPageTurns")->text(),
                 QStringLiteral("23 of 60 automatic turns used"));
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
                    ->findChild<QLabel*>("teamMemberStatus")
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

    // What a live team writes: choices that are sentences, long lanes and paths, an update
    // that is a paragraph and one that is a single unbreakable word. Nothing may leave the page.
    void longContentStaysInsideThePage() {
        QJsonObject team = fixture();
        const QString longest =
            "Theo nhóm sản phẩm: mỗi nhóm một lệnh, gom mọi món hết tồn trong 15 phút";
        QJsonObject decision = team.value("decisions").toArray().first().toObject();
        decision.insert("question", "Khi bán hàng chế biến mà hết tồn, hệ thống tự mở lệnh sản "
                                    "xuất theo cách nào để bếp không nhận quá nhiều lệnh nhỏ?");
        decision.insert("context",
                        QString("Lựa chọn này quyết định mô hình dữ liệu. ").repeated(6));
        decision.insert("choices",
                        QJsonArray{"Theo từng sản phẩm: mỗi món hết tồn mở một lệnh riêng", longest,
                                   "Không tự mở"});
        decision.insert("recommended", longest);
        team.insert("decisions", QJsonArray{decision});
        team.insert("name", "Sample release: thuế, nhập hàng loạt, bán tại quầy và báo cáo");
        team.insert(
            "goal",
            QString("Đưa Sample project vào chạy thử với thuế theo người mua. ").repeated(4));
        QJsonArray members = team.value("members").toArray();
        QJsonObject developer = members.at(1).toObject();
        developer.insert("name", "Developer 1 with a conversation title that goes on and on");
        developer.insert("provider", "Claude · Opus 5.5 · a provider label that is long");
        developer.insert("lane", "Thuế, Kế toán & Bán hàng, và mọi thứ quanh hoá đơn");
        developer.insert("paths", QJsonArray{"src/sample/", "src/sample/"});
        members.replace(1, developer);
        team.insert("members", members);
        QJsonArray tasks = team.value("tasks").toArray();
        QJsonObject first = tasks.first().toObject();
        first.insert("title",
                     "Thuế cho tổ chức trong nhập và sửa hàng loạt sản phẩm của catalogue");
        first.insert("lane", developer.value("lane"));
        tasks.replace(0, first);
        team.insert("tasks", tasks);
        start(team);
        daemon_.answers.insert(
            "team.get",
            {{"ok", true},
             {"team", team},
             {"updates",
              QJsonArray{
                  QJsonObject{
                      {"id", 1},
                      {"at", "2026-11-10T10:00:00.000Z"},
                      {"actor", "dev1"},
                      {"task_id", "T-1"},
                      {"text", QString("Đã xong phần nhập tệp, còn sửa hàng loạt. ").repeated(30)}},
                  QJsonObject{{"id", 2},
                              {"at", "2026-11-10T10:01:00.000Z"},
                              {"actor", "daemon"},
                              {"kind", "outside_lane"},
                              {"text", QString("src/sample/").repeated(20)}}}}});
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.show();
        const QString grabs = qEnvironmentVariable("KETPLUS_TEAM_PAGE_GRABS");
        for (const int width : {480, 620, 800, 1024, 1440}) {
            page.resize(width, 900);
            QTRY_COMPARE(page.updates().size(), 2);
            QCoreApplication::processEvents();
            QWidget* content = page.scrollArea()->widget();
            QVERIFY2(content->width() <= page.scrollArea()->viewport()->width(),
                     qPrintable(QStringLiteral("page %1: content is %2 wide in %3")
                                    .arg(width)
                                    .arg(content->width())
                                    .arg(page.scrollArea()->viewport()->width())));
            for (QWidget* child : content->findChildren<QWidget*>()) {
                // The table lays its own cells out and elides them.
                if (!child->isVisibleTo(content) || page.tasksTable()->isAncestorOf(child))
                    continue;
                const QRect box(child->mapTo(content, QPoint(0, 0)), child->size());
                QVERIFY2(box.left() >= 0 && box.right() < content->width(),
                         qPrintable(QStringLiteral("page %1: %2 \"%3\" spans %4..%5 of %6")
                                        .arg(width)
                                        .arg(child->metaObject()->className(), child->objectName())
                                        .arg(box.left())
                                        .arg(box.right())
                                        .arg(content->width())));
            }
            // Every column the table shows fits; it has no sideways scroll either.
            const QHeaderView* header = page.tasksTable()->horizontalHeader();
            QVERIFY2(header->length() <= page.tasksTable()->viewport()->width(),
                     qPrintable(QStringLiteral("page %1: task columns need %2 of %3")
                                    .arg(width)
                                    .arg(header->length())
                                    .arg(page.tasksTable()->viewport()->width())));
            QCOMPARE(header->isSectionHidden(4), width < 1024); // the lane goes first
            if (!grabs.isEmpty()) {
                content->resize(content->width(), content->sizeHint().height());
                QVERIFY(content->grab().save(
                    QStringLiteral("%1/team-page-%2.png").arg(grabs).arg(width)));
            }
        }
        // Sentences are rows, the recommended one first; each still answers the decision.
        auto* card = page.findChild<QFrame*>("teamDecision_D-1");
        const auto rows = card->findChildren<team_page::ChoiceRow*>("teamDecisionChoice");
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows.first()->property("choice").toString(), longest);
        QVERIFY(rows.first()->accessibleName().endsWith("recommended"));
        QVERIFY(card->findChildren<ket::Button*>("teamDecisionChoice").isEmpty());
        // A paragraph shows three lines and the rest on request.
        auto* more = page.findChildren<ket::Button*>("teamUpdateMore").last();
        auto* text = more->parentWidget()->findChild<team_page::Text*>("teamUpdate");
        QVERIFY(text->truncated() && more->isVisible());
        const int folded = text->height();
        more->click();
        QTRY_VERIFY(text->height() > folded);
        QVERIFY(!text->truncated() && more->isVisible());
        QCOMPARE(more->text(), QStringLiteral("Show less"));
        rows.at(1)->click();
        QCOMPARE(daemon_.last("team.decision.answer").value("choice").toString(),
                 QStringLiteral("Theo từng sản phẩm: mỗi món hết tồn mở một lệnh riêng"));
    }

    void pausedAndQueueBannersAndTheEmptyPage() {
        start(fixture(1, 4, true, false));
        TeamPage page(teams_.get(), "7", "/work/shop");
        auto* paused = page.findChild<ket::Banner*>("teamPausedBanner");
        QVERIFY(paused);
        QVERIFY(paused->message().contains("automatic turns"));
        paused->action()->click();
        QCOMPARE(daemon_.last("team.update").value("paused").toBool(true), false);
        auto* queue = page.findChild<ket::Banner*>("teamQueueBanner");
        QVERIFY(queue);
        QCOMPARE(queue->action()->objectName(), QStringLiteral("teamQueueAddReviewer"));
        QSignalSpy second(&page, &TeamPage::addReviewerRequested);
        queue->action()->click();
        QCOMPARE(second.first().first().toString(), QStringLiteral("7"));
        QVERIFY(!page.findChild<QLabel*>("teamWaitingHeading")); // nothing waits
        TeamPage empty(teams_.get(), "99", "/work/other");
        auto* state = empty.findChild<ket::EmptyState*>("teamPageEmpty");
        QVERIFY(state);
        QSignalSpy create(&empty, &TeamPage::newTeamRequested);
        state->action()->click();
        QCOMPARE(create.first().first().toString(), QStringLiteral("/work/other"));
    }

    // Answering sends team.decision.answer; the answer's team removes the card.
    // T-23: two reviewers, each with its own review and queue; no queue banner, and either
    // one may be removed. KETPLUS_TEAM_REVIEWERS_CAPTURE (a folder) saves the page, the New team
    // dialog with two reviewers and the second reviewer's dialog beside the mock's.
    void twoReviewersShowTheirOwnReviews_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void twoReviewersShowTheirOwnReviews() {
        QFETCH(bool, dark);
        ThemeManager theme(*qApp);
        theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
        QJsonObject team = fixture(1, 4, false, false);
        QJsonArray members = team.value("members").toArray();
        members.append(member("rev2", "reviewer", "Review 2"));
        team.insert("members", members);
        team.insert(
            "reviews",
            QJsonArray{
                QJsonObject{
                    {"reviewer", "rev"}, {"reviewing", "T-1"}, {"queue", QJsonArray{"T-3"}}},
                QJsonObject{{"reviewer", "rev2"}, {"reviewing", ""}, {"queue", QJsonArray{}}}});
        team.insert("reviewing", "T-1");
        start(team);
        QWidget window;
        window.resize(1440, 1200);
        TeamPage page(teams_.get(), "7", "/work/shop", &window);
        page.resize(window.size());
        window.show();
        QTRY_VERIFY(page.findChild<QWidget*>("teamMember_rev2"));
        QVERIFY(!page.findChild<ket::Banner*>("teamQueueBanner"));
        QVERIFY(page.findChild<QLabel*>("teamMembersHeading")
                    ->parentWidget()
                    ->accessibleName()
                    .endsWith("4 developers · 2 reviewers"));
        const auto status = [&](const char* session) {
            return labels(page.findChild<QWidget*>(QStringLiteral("teamMember_") + session),
                          "teamMemberStatus")
                .value(0);
        };
        QCOMPARE(status("rev"), QStringLiteral("Reviewing T-1 +1"));
        QCOMPARE(status("rev2"), QStringLiteral("Nothing queued"));
        auto* remove = page.findChild<QWidget*>("teamMember_rev2")
                           ->findChild<ket::Button*>("teamMemberRemove");
        QVERIFY(remove);
        QVERIFY(remove->isEnabled());

        const QString capture = qEnvironmentVariable("KETPLUS_TEAM_REVIEWERS_CAPTURE");
        const QString name = dark ? QStringLiteral("dark") : QStringLiteral("light");
        if (!capture.isEmpty())
            window.grab().save(QStringLiteral("%1/app-team-two-%2.png").arg(capture, name));
        const QVariantList models{QVariantMap{{"provider", "claude"},
                                              {"model", "opus"},
                                              {"displayName", "Opus"},
                                              {"isDefault", true}}};
        auto* create = NewTeamForm::present(&page, teams_.get(), "/work/shop", models);
        auto* count = create->findChild<ket::SegmentedControl*>("teamReviewerCount");
        QTRY_VERIFY(count && count->isVisible());
        count->setCurrentIndex(1);
        QApplication::processEvents();
        if (!capture.isEmpty())
            window.grab().save(QStringLiteral("%1/app-new-team-%2.png").arg(capture, name));
        create->reject();
        QJsonObject one = fixture(1, 4, false, false);
        auto* second =
            AddMemberForm::present(&page, teams_.get(), one, models, QStringLiteral("reviewer"));
        QTRY_VERIFY(second->findChild<AddMemberForm*>()->isVisible());
        QCOMPARE(second->findChild<AddMemberForm*>()->role(), QStringLiteral("reviewer"));
        // The card fits the form once its help lines are wrapped: nothing scrolls.
        QTRY_COMPARE(
            second->findChild<QScrollArea*>("teamDialogScroll")->verticalScrollBar()->maximum(), 0);
        // The card follows its body on the next event loop pass: the body ends above the
        // footer's buttons.
        const auto* body = second->findChild<QScrollArea*>("teamDialogScroll");
        const auto* add = second->findChild<ket::Button*>("addMemberButton");
        QTRY_VERIFY2(body->mapTo(second, QPoint(0, body->height())).y() <=
                         add->mapTo(second, QPoint()).y(),
                     qPrintable(QStringLiteral("body ends %1, Add starts %2, card %3")
                                    .arg(body->mapTo(second, QPoint(0, body->height())).y())
                                    .arg(add->mapTo(second, QPoint()).y())
                                    .arg(second->card()->height())));
        if (!capture.isEmpty())
            window.grab().save(QStringLiteral("%1/app-add-reviewer-%2.png").arg(capture, name));
        second->reject();
    }

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

    // T-55: the last choice is "Other…", a field on the card. Enter sends, Shift+Enter adds a
    // line, Esc and Cancel put the card back; a refusal shows on the card and keeps the text.
    void otherWritesTheAnswerOnTheCard() {
        start(fixture());
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1100, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        const auto card = [&] { return page.findChild<QFrame*>("teamDecision_D-1"); };
        const auto field = [&] {
            return card() ? card()->findChild<ket::TextArea*>("teamDecisionOtherText") : nullptr;
        };
        const auto button = [&](const char* name) {
            return card()->findChild<QAbstractButton*>(QLatin1String(name));
        };
        // The conversation is not where the card is answered any more.
        QVERIFY(!card()->findChild<QWidget*>("teamDecisionInConversation"));
        for (auto* any : card()->findChildren<QAbstractButton*>())
            QVERIFY(!any->text().contains("conversation"));
        QVERIFY(!field());
        const auto open = [&] {
            QAbstractButton* other = button("teamDecisionOther");
            QVERIFY(other && other->isEnabled());
            QVERIFY(other->text().startsWith("Other"));
            other->click();
            QTRY_VERIFY(field());
            // The window's focus child: offscreen, the page need not be the active window.
            QTRY_COMPARE(page.focusWidget(), static_cast<QWidget*>(field()->editor()));
            QVERIFY(!button("teamDecisionOther"));
        };
        open();
        // Nothing to send while empty or blank: Send waits and Enter sends nothing.
        QVERIFY(!button("teamDecisionOtherSend")->isEnabled());
        field()->setText("   ");
        QVERIFY(!button("teamDecisionOtherSend")->isEnabled());
        QTest::keyClick(field()->editor(), Qt::Key_Return);
        QCOMPARE(daemon_.count("team.decision.answer"), 0);
        // Esc puts the card back, with its choices and Other.
        QTest::keyClick(field()->editor(), Qt::Key_Escape);
        QTRY_VERIFY(!field());
        QCOMPARE(card()->findChildren<ket::Button*>("teamDecisionChoice").size(), 2);
        open();
        button("teamDecisionOtherCancel")->click();
        QTRY_VERIFY(!field());
        QVERIFY(button("teamDecisionOther"));

        open();
        QTest::keyClicks(field()->editor(), "Neither: use Adyen");
        QTest::keyClick(field()->editor(), Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClicks(field()->editor(), "for the fees");
        QCOMPARE(field()->text(), QStringLiteral("Neither: use Adyen\nfor the fees"));
        QVERIFY(button("teamDecisionOtherSend")->isEnabled());
        const QString closed = "The decision D-1 was already answered.";
        daemon_.answers.insert(
            "team.decision.answer",
            {{"ok", false}, {"error_code", "decision_closed"}, {"error_message", closed}});
        QTest::keyClick(field()->editor(), Qt::Key_Return);
        QCOMPARE(daemon_.count("team.decision.answer"), 1);
        QJsonObject sent = daemon_.last("team.decision.answer");
        QCOMPARE(sent.value("decision_id").toString(), QStringLiteral("D-1"));
        QCOMPARE(sent.value("choice").toString(),
                 QStringLiteral("Neither: use Adyen\nfor the fees"));
        // Refused: the reason on the card, the text as it was typed.
        auto* error = card()->findChild<QLabel*>("teamDecisionError");
        QTRY_VERIFY(error->isVisible());
        QVERIFY(error->text().contains(closed));
        QCOMPARE(field()->text(), QStringLiteral("Neither: use Adyen\nfor the fees"));
        QVERIFY(button("teamDecisionOtherSend")->isEnabled());

        // A refused payload takes the same way: the reason on the card, the text kept.
        const QString invalid = "The answer is too long.";
        daemon_.answers.insert(
            "team.decision.answer",
            {{"ok", false}, {"error_code", "invalid_payload"}, {"error_message", invalid}});
        button("teamDecisionOtherSend")->click();
        QCOMPARE(daemon_.count("team.decision.answer"), 2);
        QTRY_VERIFY(card()->findChild<QLabel*>("teamDecisionError")->text().contains(invalid));
        QCOMPARE(field()->text(), QStringLiteral("Neither: use Adyen\nfor the fees"));

        // Sent again and taken: the decision leaves Waiting for you, and the answer is in Updates.
        const QJsonObject answered = fixture(2, 2, false, false);
        daemon_.answers.insert("team.decision.answer", {{"ok", true}, {"team", answered}});
        daemon_.answers.insert(
            "team.get",
            {{"ok", true},
             {"team", answered},
             {"updates", QJsonArray{QJsonObject{{"id", 3},
                                                {"at", "2026-11-10T10:02:00.000Z"},
                                                {"text", "D-1 answered: Neither: use Adyen"}}}}});
        button("teamDecisionOtherSend")->click();
        QCOMPARE(daemon_.count("team.decision.answer"), 3);
        QTRY_VERIFY(!card());
        QTRY_VERIFY(labels(&page, "teamUpdate").join('\n').contains("D-1 answered: Neither"));
    }

    // T-55 review M1: the team changes while the answer is being written. The page rebuilds, and
    // the new field has the text, the cursor, the selection and the keyboard where they were. A
    // word the input method is still composing holds the rebuild until it is committed.
    void aTeamChangeKeepsTheAnswerBeingWritten() {
        start(fixture());
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1100, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        const auto field = [&] {
            auto* card = page.findChild<QFrame*>("teamDecision_D-1");
            return card ? card->findChild<ket::TextArea*>("teamDecisionOtherText") : nullptr;
        };
        page.findChild<QFrame*>("teamDecision_D-1")
            ->findChild<QAbstractButton*>("teamDecisionOther")
            ->click();
        QTRY_VERIFY(field());
        QTRY_COMPARE(page.focusWidget(), static_cast<QWidget*>(field()->editor()));
        QTest::keyClicks(field()->editor(), "Neither: use Adyen");
        // Back in the middle: "use" selected, from its end to its start.
        QTextCursor cursor = field()->editor()->textCursor();
        cursor.setPosition(12);
        cursor.setPosition(9, QTextCursor::KeepAnchor);
        field()->editor()->setTextCursor(cursor);

        int revision = 5;
        const auto change = [&] {
            QJsonObject next = fixture(revision++);
            next.insert("name", QStringLiteral("Shop %1").arg(revision));
            teams_->applyEvent("team.changed", {{"team", next}});
            QApplication::processEvents();
        };
        QPointer<ket::TextArea> before(field());
        change();
        QVERIFY(!before); // the content was rebuilt
        QVERIFY(field());
        QCOMPARE(field()->text(), QStringLiteral("Neither: use Adyen"));
        QCOMPARE(field()->editor()->textCursor().anchor(), 12);
        QCOMPARE(field()->editor()->textCursor().position(), 9);
        QCOMPARE(field()->editor()->textCursor().selectedText(), QStringLiteral("use"));
        QCOMPARE(page.focusWidget(), static_cast<QWidget*>(field()->editor()));
        // The next keys replace the selection, not land at the end.
        QTest::keyClicks(field()->editor(), "pay with");
        QCOMPARE(field()->text(), QStringLiteral("Neither: pay with Adyen"));
        QCOMPARE(field()->editor()->textCursor().position(), 17);

        // Mid-composition the field stays as it is; the rebuild comes once the word is in.
        QInputMethodEvent composing(QStringLiteral("Vi"), {});
        QCoreApplication::sendEvent(field()->editor(), &composing);
        before = field();
        change();
        QVERIFY(before);
        QTest::qWait(300);
        QVERIFY(before); // still composing: the rebuild waits again
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral(" Việt"));
        QCoreApplication::sendEvent(field()->editor(), &commit);
        QTRY_VERIFY(!before);
        QCOMPARE(field()->text(), QStringLiteral("Neither: pay with Việt Adyen"));
        QCOMPARE(field()->editor()->textCursor().position(), 22);
        QCOMPARE(page.findChild<QLabel*>("teamPageName")->text(), QStringLiteral("Shop 7"));
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
        // The handover is a dialog of its own, opened from the task's actions.
        QVERIFY(!window.findChild<ket::Button*>("teamHandoverButton"));
        QVERIFY(dialog->findChild<ket::Button*>("teamHandoverOpen")->isVisibleTo(dialog));
        QVERIFY(dialog->findChild<ket::Button*>("teamTaskOpenOwner")->isVisibleTo(dialog));
        dialog->findChild<ket::Button*>("teamHandoverOpen")->click();
        QPointer<ket::Dialog> handover = window.findChild<ket::Dialog*>("teamHandoverDialog");
        QVERIFY(handover && handover->isOpen());
        // Cancelled, it opens again as a new dialog with the same fields.
        handover->reject();
        QTRY_VERIFY(!handover);
        QVERIFY(body->handoverOwner());
        dialog->findChild<ket::Button*>("teamHandoverOpen")->click();
        handover = window.findChild<ket::Dialog*>("teamHandoverDialog");
        QVERIFY(handover && handover->isOpen());
        QVERIFY(body->handoverReason()->isVisibleTo(&window));
        window.findChild<ket::Button*>("teamHandoverButton")->click();
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
        window.findChild<ket::Button*>("teamHandoverButton")->click();
        const QJsonObject sent = daemon_.last("team.task.handover");
        QCOMPARE(sent.value("task_id").toString(), QStringLiteral("T-1"));
        QCOMPARE(sent.value("owner").toString(), QStringLiteral("dev2"));
        QCOMPARE(sent.value("reason").toString(), QStringLiteral("Dev 1 is on T-3"));
        QVERIFY(body->error().isEmpty());
        // The row and the open dialog show the new owner.
        QTRY_VERIFY(page.tasksTable()->item(0, 3)->text().startsWith("Dev 2"));
        QVERIFY(dialog->isOpen());
        QVERIFY(dialog->findChild<QLabel*>("teamTaskOwner")->text().contains("Dev 2"));
        // The handover closed itself and left its fields with the task.
        QTRY_VERIFY(!handover);
        QVERIFY(body->handoverOwner());
        // And after a handover that went through: Dev 1 can take it back.
        dialog->findChild<ket::Button*>("teamHandoverOpen")->click();
        handover = window.findChild<ket::Dialog*>("teamHandoverDialog");
        QVERIFY(handover && handover->isOpen());
        QCOMPARE(body->handoverOwner()->currentData().toString(), QStringLiteral("dev1"));
        handover->reject();
        QTRY_VERIFY(!handover);
        QSignalSpy asked(&page, &TeamPage::conversationRequested);
        dialog->findChild<ket::Button*>("teamTaskOpenOwner")->click();
        QCOMPARE(asked.size(), 1);
        QCOMPARE(asked.first().first().toString(), QStringLiteral("dev2"));
    }

    // An answer that arrives after its handover dialog was cancelled belongs to that dialog:
    // it neither closes the one opened since nor clears what the user wrote there.
    // ADR 0115: the task's history comes from team.task.get a page at a time.
    void theTaskDialogReadsItsHistoryPageByPage() {
        start(fixture());
        QJsonObject paged = task("T-1", "in_progress", "dev1");
        QJsonArray newest;
        for (int index = 70; index < 120; ++index)
            newest.append(QJsonObject{{"at", "2026-11-10T10:00:00.000Z"},
                                      {"actor", "daemon"},
                                      {"text", QStringLiteral("Entry %1").arg(index)}});
        paged.insert("history", newest);
        paged.insert("history_total", 120);
        paged.insert("history_start", 70);
        daemon_.answers.insert("team.task.get", {{"ok", true}, {"task", paged}});
        QWidget window;
        window.resize(1024, 640);
        TeamPage page(teams_.get(), "7", "/work/shop", &window);
        window.show();
        auto* dialog = page.openTask("T-1");
        QCOMPARE(daemon_.last("team.task.get").value("task_id").toString(), QStringLiteral("T-1"));
        QTRY_COMPARE(labels(dialog, "teamTaskHistory").size(), 50);
        QVERIFY(labels(dialog, "teamTaskHistory").last().contains("Entry 119"));
        auto* earlier = dialog->findChild<ket::Button*>("teamTaskHistoryEarlier");
        QVERIFY(earlier);
        QJsonObject older = paged;
        QJsonArray first;
        for (int index = 20; index < 70; ++index)
            first.append(QJsonObject{{"at", "2026-11-10T10:00:00.000Z"},
                                     {"actor", "daemon"},
                                     {"text", QStringLiteral("Entry %1").arg(index)}});
        older.insert("history", first);
        older.insert("history_start", 20);
        daemon_.answers.insert("team.task.get", {{"ok", true}, {"task", older}});
        earlier->click();
        QCOMPARE(daemon_.last("team.task.get").value("history_before").toInt(), 70);
        QTRY_COMPARE(labels(dialog, "teamTaskHistory").size(), 100);
        QVERIFY(labels(dialog, "teamTaskHistory").first().contains("Entry 20"));
        QVERIFY(labels(dialog, "teamTaskHistory").last().contains("Entry 119"));
    }

    // ADR 0115: older Updates come a page at a time.
    void olderUpdatesComeOnRequest() {
        start(fixture());
        QJsonObject get = daemon_.answers.value("team.get");
        get.insert("updates_has_more", true);
        daemon_.answers.insert("team.get", get);
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1024, 640);
        page.show();
        QTRY_VERIFY(page.findChild<ket::Button*>("teamUpdatesOlder"));
        daemon_.answers.insert(
            "team.updates.list",
            {{"ok", true},
             {"updates",
              QJsonArray{QJsonObject{
                  {"id", 0}, {"at", "2026-11-10T09:00:00.000Z"}, {"text", "Team created"}}}},
             {"has_more", false}});
        page.findChild<ket::Button*>("teamUpdatesOlder")->click();
        QCOMPARE(daemon_.last("team.updates.list").value("before_id").toInt(), 1);
        QTRY_COMPARE(labels(&page, "teamUpdate").size(), 3);
        QVERIFY(labels(&page, "teamUpdate").last().contains("Team created"));
        QVERIFY(!page.findChild<ket::Button*>("teamUpdatesOlder"));
    }

    void aLateHandoverAnswerLeavesANewDialogAlone() {
        start(fixture());
        QWidget window;
        window.resize(1024, 640);
        TeamPage page(teams_.get(), "7", "/work/shop", &window);
        window.show();
        auto* dialog = page.openTask("T-1");
        auto* body = dialog->findChild<TeamTaskDialog*>();
        QPointer<ket::Dialog> first = body->openHandover();
        QVERIFY(first);
        body->handoverReason()->setText("First reason");
        daemon_.hold = true;
        body->handOver();
        QCOMPARE(daemon_.late.size(), 1);
        first->reject();
        QTRY_VERIFY(!first);
        QPointer<ket::Dialog> second = body->openHandover();
        QVERIFY(second && second->isOpen());
        body->handoverReason()->setText("A new draft");
        daemon_.hold = false;
        daemon_.late.takeFirst()();
        QCoreApplication::processEvents();
        QVERIFY(second && second->isOpen());
        QCOMPARE(body->handoverReason()->text(), QStringLiteral("A new draft"));
        QVERIFY(body->error().isEmpty());
        second->reject();
        dialog->reject();
    }

    // A leader writes a task as Markdown, often long: the dialog renders it, cuts it and keeps
    // itself and its actions inside the window.
    void aLongDescriptionIsCutAndStaysInsideTheWindow() {
        QJsonObject team = fixture(2);
        QJsonArray tasks = team.value("tasks").toArray();
        QJsonObject long1 = tasks.at(0).toObject();
        long1.insert("description",
                     QString("## Scope\n\nMove `src/sample/` to the new pipeline.\n\n") +
                         QString("- Keep the public API of `ImportJob` as it is\n").repeated(14) +
                         "\n```\nnpm run test -- import\n```\n");
        long1.insert("worktree", QString("/work/shop/.worktrees/") + QString("deep/").repeated(30));
        tasks.replace(0, long1);
        team.insert("tasks", tasks);
        start(team);
        const QString grabs = qEnvironmentVariable("KETPLUS_TEAM_PAGE_GRABS");
        for (const QSize size : {QSize(1024, 640), QSize(520, 560)}) {
            QWidget window;
            window.resize(size);
            TeamPage page(teams_.get(), "7", "/work/shop", &window);
            page.resize(size);
            window.show();
            auto* dialog = page.openTask("T-1");
            auto* body = dialog->findChild<TeamTaskDialog*>();
            QCoreApplication::processEvents();
            auto* all = dialog->findChild<ket::Button*>("teamTaskDescriptionAll");
            QVERIFY(all && all->isVisibleTo(dialog));
            QCOMPARE(all->text(), QStringLiteral("Show all"));
            auto* clip = dialog->findChild<QWidget*>("teamTaskDescriptionClip");
            const int cut = clip->height();
            QVERIFY(cut < 200);
            if (!grabs.isEmpty())
                window.grab().save(
                    QStringLiteral("%1/team-task-%2.png").arg(grabs).arg(size.width()));
            // Everything the dialog shows is inside the window, the actions included.
            const QRect frame(QPoint(0, 0), window.size());
            for (const auto* button : dialog->findChildren<ket::Button*>())
                if (button->isVisibleTo(&window) && !body->isAncestorOf(button))
                    QVERIFY2(
                        frame.contains(QRect(button->mapTo(&window, QPoint(0, 0)), button->size())),
                        qPrintable(button->text()));
            auto* details = dialog->findChild<QWidget*>("teamTaskDetails");
            QVERIFY(details->width() <= body->width());
            all->click();
            QCoreApplication::processEvents();
            all = dialog->findChild<ket::Button*>("teamTaskDescriptionAll");
            QCOMPARE(all->text(), QStringLiteral("Show less"));
            QVERIFY(dialog->findChild<QWidget*>("teamTaskDescriptionClip")->height() > cut);
            if (!grabs.isEmpty())
                window.grab().save(
                    QStringLiteral("%1/team-task-%2-all.png").arg(grabs).arg(size.width()));
            dialog->reject();
        }
    }

    // ADR 0114, open point 2: the header's turns and the team's budget dialog, in both themes,
    // inside the window. KETPLUS_TEAM_BUDGET_CAPTURE (a folder) saves them beside the mock's.
    void theTeamsBudgetFitsInBothThemes_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void theTeamsBudgetFitsInBothThemes() {
        QFETCH(bool, dark);
        ThemeManager theme(*qApp);
        theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
        start(fixture());
        QWidget window;
        window.resize(1440, 900);
        TeamPage page(teams_.get(), "7", "/work/shop", &window);
        page.resize(window.size());
        window.show();
        auto* turns = page.findChild<QLabel*>("teamPageTurns");
        QTRY_VERIFY(turns && turns->isVisible());
        const QRect frame(QPoint(0, 0), window.size());
        QVERIFY(frame.contains(QRect(turns->mapTo(&window, QPoint()), turns->size())));
        const QString capture = qEnvironmentVariable("KETPLUS_TEAM_BUDGET_CAPTURE");
        const QString name = dark ? QStringLiteral("dark") : QStringLiteral("light");
        auto* dialog = TeamTurnBudgetForm::present(&page, teams_.get(), *teams_->team("7"));
        auto* field = dialog->findChild<ket::TextField*>("teamTurnBudget");
        QTRY_VERIFY(field && field->isVisible());
        QCOMPARE(field->text(), QStringLiteral("60"));
        QVERIFY(frame.contains(QRect(field->mapTo(&window, QPoint()), field->size())));
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture + "/app-team-budget-" + name + ".png"));
        dialog->reject();
    }

    // ADR 0114: a long goal on the team page, folded under the heading with Show more and its
    // mention named after its conversation, as the mock's team page, in both themes.
    // KETPLUS_TEAM_GOAL_CAPTURE (a folder) saves it folded and open beside the mock's.
    void theFoldedGoalFitsInBothThemes_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void theFoldedGoalFitsInBothThemes() {
        QFETCH(bool, dark);
        ThemeManager theme(*qApp);
        theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
        QJsonObject team = fixture();
        team.insert("goal", QStringLiteral(
                                "Ship the new navigation, the repository picker and Settings.\n\n"
                                "Context: the old sidebar confused people who work in several "
                                "repositories. @agent/8213 rebuilt the picker and knows its "
                                "edge cases.\n\nOut of scope: the account flow.\n\n"
                                "Done when every screen matches the mock in light and dark."));
        start(team);
        QWidget window;
        window.resize(1280, 860);
        TeamPage page(teams_.get(), "7", "/work/shop", &window);
        page.setConversationDirectory([] {
            return QVariantList{
                QVariantMap{{"number", "8213"}, {"title", "Finish the repository picker"}}};
        });
        page.resize(window.size());
        window.show();
        // The page rebuilds its sections when the team changes, so the goal is looked up again.
        const auto goal = [&page] { return page.findChild<ket::ExpandableText*>("teamPageGoal"); };
        QTRY_VERIFY(goal() && goal()->isVisible() && goal()->text().contains("@agent/8213"));
        QTest::qWait(100);
        QVERIFY(goal()->isFoldable());
        QCOMPARE(goal()->visibleLineCount(), team_goal::foldedLines);
        QVERIFY(goal()->accessibleDescription().contains("Finish the repository picker"));
        const QRect frame(QPoint(0, 0), window.size());
        QVERIFY(frame.contains(QRect(goal()->mapTo(&window, QPoint()), goal()->size())));
        const QString capture = qEnvironmentVariable("KETPLUS_TEAM_GOAL_CAPTURE");
        const QString name = dark ? QStringLiteral("dark") : QStringLiteral("light");
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture + "/app-team-page-" + name + ".png"));
        const int folded = goal()->height();
        goal()->setExpanded(true);
        QTRY_VERIFY(goal() && goal()->visibleLineCount() > team_goal::foldedLines);
        // The page makes room for the whole goal and Show less.
        QTRY_VERIFY(goal()->height() >= goal()->heightForWidth(goal()->width()));
        QVERIFY(goal()->height() > folded);
        QVERIFY(page.findChild<QWidget*>("teamPageFacts")->geometry().top() >=
                goal()->geometry().bottom());
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture + "/app-team-page-open-" + name + ".png"));
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

    void anotherTeamsChangeLeavesThePageAlone() {
        start(fixture());
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(900, 700);
        page.show();
        QApplication::processEvents();
        const QPointer<QWidget> content(page.scrollArea()->widget());
        QJsonObject other = fixture(9);
        other.insert("id", "8");
        other.insert("name", "Billing");
        teams_->applyEvent("team.changed", {{"team", other}});
        QApplication::processEvents();
        QVERIFY(content);
        QCOMPARE(page.scrollArea()->widget(), content.data());
        // Its own change still rebuilds it.
        QJsonObject next = fixture(5);
        next.insert("name", "Shop 2");
        teams_->applyEvent("team.changed", {{"team", next}});
        QVERIFY(page.scrollArea()->widget() != content.data());
        QCOMPARE(page.findChild<QLabel*>("teamPageName")->text(), QStringLiteral("Shop 2"));
    }

    void aLiveUpdateKeepsTheFocusAndTheCurrentTask() {
        start(fixture());
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(900, 700);
        page.show();
        QApplication::processEvents();
        QVERIFY(page.tasksTable()->rowCount() > 0);
        page.tasksTable()->setCurrentCell(0, 0);
        const QString task = page.tasksTable()->item(0, 0)->text();
        const QList<QAbstractButton*> open = page.findChildren<QAbstractButton*>("teamMemberOpen");
        QVERIFY(open.size() > 1);
        const QString row = open.last()->parentWidget()->objectName();
        QVERIFY(row.startsWith("teamMember_"));
        const QPointer<QAbstractButton> before(open.last());
        before->setFocus();
        QCOMPARE(page.window()->focusWidget(), open.last());
        QJsonObject next = fixture(5);
        next.insert("name", "Shop 2");
        teams_->applyEvent("team.changed", {{"team", next}});
        QApplication::processEvents();
        QVERIFY(!before); // the content was rebuilt
        QWidget* focused = page.window()->focusWidget();
        QVERIFY(focused);
        QCOMPARE(focused->objectName(), QStringLiteral("teamMemberOpen"));
        QCOMPARE(focused->parentWidget()->objectName(), row);
        QCOMPARE(page.tasksTable()->item(page.tasksTable()->currentRow(), 0)->text(), task);
    }

    // A banner's button keeps the keyboard through a live update: Space still resumes the team.
    // So does a part without a name, which is found again by its place in the banner.
    void aLiveUpdateKeepsTheFocusOnABannersButton() {
        start(fixture(1, 4, true, false));
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(900, 700);
        page.show();
        QApplication::processEvents();
        for (const bool named : {true, false}) {
            const QPointer<QAbstractButton> before(
                page.findChild<ket::Banner*>("teamPausedBanner")->action());
            QCOMPARE(before->objectName(), QStringLiteral("teamPausedResume"));
            if (!named)
                before->setObjectName(QString());
            before->setFocus();
            QCOMPARE(page.window()->focusWidget(), before.data());
            QJsonObject next = fixture(named ? 5 : 6, 4, true, false);
            next.insert("name", named ? "Shop 2" : "Shop 3");
            teams_->applyEvent("team.changed", {{"team", next}});
            QApplication::processEvents();
            QVERIFY(!before); // the content was rebuilt
            QWidget* focused = page.window()->focusWidget();
            QVERIFY(focused);
            QCOMPARE(focused, page.findChild<ket::Banner*>("teamPausedBanner")->action());
            const int sent = daemon_.count("team.update");
            QTest::keyClick(focused, Qt::Key_Space);
            QCOMPARE(daemon_.count("team.update"), sent + 1);
            QCOMPARE(daemon_.last("team.update").value("paused").toBool(true), false);
        }
        QVERIFY(page.findChild<QAbstractButton*>("teamQueueAddReviewer"));
    }
};

QTEST_MAIN(TeamPageTest)
#include "TeamPageTest.moc"
