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
#include "teams/TeamPageParts.h"
#include "teams/TeamTaskDialog.h"

#include <QApplication>
#include <QHeaderView>
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
        QCOMPARE(page.findChild<QLabel*>("teamPageRepository")->text(), QStringLiteral("shop"));
        QCOMPARE(page.findChild<QLabel*>("teamPageBranch")->text(), QStringLiteral("team/shop"));
        QCOMPARE(page.findChild<QLabel*>("teamPageMerges")->text(),
                 QStringLiteral("The leader merges tasks"));
        QCOMPARE(page.findChild<QLabel*>("teamPageProgress")->text(),
                 QStringLiteral("1 of 4 tasks done"));
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
        team.insert("name", "Kingfruit Pilot: thuế, nhập hàng loạt, bán tại quầy và báo cáo");
        team.insert("goal",
                    QString("Đưa Kingfruit vào chạy thử với thuế theo người mua. ").repeated(4));
        QJsonArray members = team.value("members").toArray();
        QJsonObject developer = members.at(1).toObject();
        developer.insert("name", "Developer 1 with a conversation title that goes on and on");
        developer.insert("provider", "Claude · Opus 5.5 · a provider label that is long");
        developer.insert("lane", "Thuế, Kế toán & Bán hàng, và mọi thứ quanh hoá đơn");
        developer.insert("paths", QJsonArray{"ketsuite/packages/ketsuite/src/modules/account/",
                                             "ketsuite/packages/ketsuite/src/modules/product/"});
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
                              {"text", QString("ketsuite/packages/").repeated(20)}}}}});
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
        // The handover is a dialog of its own, opened from the task's actions.
        QVERIFY(!window.findChild<ket::Button*>("teamHandoverButton"));
        QVERIFY(dialog->findChild<ket::Button*>("teamHandoverOpen")->isVisibleTo(dialog));
        QVERIFY(dialog->findChild<ket::Button*>("teamTaskOpenOwner")->isVisibleTo(dialog));
        dialog->findChild<ket::Button*>("teamHandoverOpen")->click();
        QPointer<ket::Dialog> handover = window.findChild<ket::Dialog*>("teamHandoverDialog");
        QVERIFY(handover && handover->isOpen());
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
        QTRY_VERIFY(!handover || !handover->isOpen());
        QVERIFY(body->handoverOwner());
        QSignalSpy asked(&page, &TeamPage::conversationRequested);
        dialog->findChild<ket::Button*>("teamTaskOpenOwner")->click();
        QCOMPARE(asked.size(), 1);
        QCOMPARE(asked.first().first().toString(), QStringLiteral("dev2"));
    }

    // A leader writes a task as Markdown, often long: the dialog renders it, cuts it and keeps
    // itself and its actions inside the window.
    void aLongDescriptionIsCutAndStaysInsideTheWindow() {
        QJsonObject team = fixture(2);
        QJsonArray tasks = team.value("tasks").toArray();
        QJsonObject long1 = tasks.at(0).toObject();
        long1.insert(
            "description",
            QString("## Scope\n\nMove `ketsuite/packages/import` to the new pipeline.\n\n") +
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
