// T-6: the team page and the task dialog follow team.changed without building everything again:
// a change elsewhere builds nothing, a burst of changes builds once, and a section is built again
// only when what it shows changed. Scroll, focus, an open dialog, the theme and the zoom keep
// working. Run with QT_QPA_PLATFORM=offscreen.
#include "KetAppearance.h"
#include "KetBanner.h"
#include "KetDialog.h"
#include "KetStyle.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPage.h"
#include "teams/TeamTaskDialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QJsonArray>
#include <QLabel>
#include <QPointer>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTableWidget>
#include <QtTest>

#include <functional>

using namespace ketplus;
using Section = TeamPage::Section;

namespace {

QJsonObject member(const QString& session, const QString& role, const QString& name,
                   const QString& lane = {}) {
    QJsonObject json{{"session_id", session}, {"role", role}, {"name", name}, {"number", "1"}};
    if (role == "developer")
        json.insert("lane", lane), json.insert("paths", QJsonArray{"src/" + lane.toLower()});
    return json;
}

QJsonObject entry(const QString& text) {
    return {
        {"at", "2026-11-10T10:00:00.000Z"}, {"actor", "lead"}, {"kind", "note"}, {"text", text}};
}

QJsonObject task(const QString& id, const QString& state, const QString& owner) {
    return {{"id", id},
            {"title", "Task " + id},
            {"state", state},
            {"owner", owner},
            {"lane", "Web"},
            {"criteria", QJsonArray{QJsonObject{{"text", "Works"}, {"met", true}},
                                    QJsonObject{{"text", "Tested"}, {"met", false}}}},
            {"criteria_met", 1},
            {"outside_lane", QJsonArray{}},
            {"depends_on", QJsonArray{}},
            {"history", QJsonArray{entry("Created")}}};
}

QJsonObject fixture(const int revision = 1) {
    QJsonArray members{
        member("lead", "leader", "Lead"), member("dev1", "developer", "Dev 1", "Web"),
        member("dev2", "developer", "Dev 2", "Api"), member("rev", "reviewer", "Review")};
    QJsonArray tasks;
    for (int index = 1; index <= 12; ++index)
        tasks.append(task(QStringLiteral("T-%1").arg(index), index == 1 ? "in_progress" : "todo",
                          index % 2 ? "dev1" : "dev2"));
    return {{"id", "7"},
            {"name", "Shop"},
            {"goal", "Checkout in one page"},
            {"repository", "/work/shop"},
            {"integration_branch", "team/shop"},
            {"merge_policy", "leader"},
            {"paused", false},
            {"turn_budget", 60},
            {"automatic_turns", 23},
            {"members", members},
            {"tasks", tasks},
            {"decisions", QJsonArray{}},
            {"reviews", QJsonArray{QJsonObject{
                            {"reviewer", "rev"}, {"reviewing", ""}, {"queue", QJsonArray{}}}}},
            {"revision", revision}};
}

// `team` with its task `id` changed by `edit`.
QJsonObject withTask(QJsonObject team, const QString& id,
                     const std::function<void(QJsonObject&)>& edit) {
    QJsonArray tasks = team.value("tasks").toArray();
    for (int index = 0; index < tasks.size(); ++index)
        if (tasks.at(index).toObject().value("id").toString() == id) {
            QJsonObject changed = tasks.at(index).toObject();
            edit(changed);
            tasks.replace(index, changed);
        }
    team.insert("tasks", tasks);
    return team;
}

// What a turn starting changes: the reviewer is busy and the team counts the turn.
QJsonObject withActivity(QJsonObject team) {
    team.insert("reviews",
                QJsonArray{QJsonObject{
                    {"reviewer", "rev"}, {"reviewing", "T-2"}, {"queue", QJsonArray{}}}});
    team.insert("outbox", 1);
    team.insert("changed_at", "2026-11-23T03:00:00.000Z");
    return team;
}

QJsonObject historyAnswer(const QJsonArray& history) {
    return {{"ok", true},
            {"task", QJsonObject{{"id", "T-1"},
                                 {"history", history},
                                 {"history_start", 0},
                                 {"history_total", history.size()}}}};
}

// Counts the widgets shown as windows of their own while it watches.
class WindowWatch final : public QObject {
  public:
    explicit WindowWatch(QWidget* ignored) : ignored_(ignored) { qApp->installEventFilter(this); }
    ~WindowWatch() override { qApp->removeEventFilter(this); }
    QStringList shown;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Show && watched->isWidgetType())
            if (auto* widget = static_cast<QWidget*>(watched);
                widget->isWindow() && widget != ignored_)
                shown.append(QString::fromLatin1(widget->metaObject()->className()) + u' ' +
                             widget->objectName());
        return QObject::eventFilter(watched, event);
    }

  private:
    QWidget* ignored_;
};

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
            sent.cbegin(), sent.cend(), [&](const auto& sent) { return sent.first == type; }));
    }
};

} // namespace

class TeamPageRefreshTest final : public QObject {
    Q_OBJECT

  private:
    FakeDaemon daemon_;
    std::unique_ptr<TeamDirectory> teams_;
    int revision_{10};

    void start() {
        daemon_ = {};
        const QJsonObject team = fixture();
        daemon_.answers.insert("team.list", {{"ok", true}, {"teams", QJsonArray{team}}});
        // An older revision: the directory keeps what the events said.
        daemon_.answers.insert("team.get", {{"ok", true}, {"team", team}, {"updates", {}}});
        daemon_.answers.insert("team.task.get",
                               historyAnswer({entry("Created"), entry("Assigned")}));
        teams_ = std::make_unique<TeamDirectory>();
        teams_->setTransport(daemon_.transport());
        teams_->setAvailable(true);
    }

    // Sends `team` as the daemon's next revision of it, without running the event loop.
    void send(QJsonObject team) {
        team.insert("revision", ++revision_);
        teams_->applyEvent("team.changed", {{"team", team}});
    }

    QHash<Section, int> builds(const TeamPage& page) const {
        QHash<Section, int> counted;
        for (int section = 0; section < TeamPage::kSections; ++section)
            counted.insert(static_cast<Section>(section),
                           page.sectionBuildsForTesting(static_cast<Section>(section)));
        return counted;
    }

    static void settle() {
        for (int round = 0; round < 3; ++round)
            QCoreApplication::processEvents();
    }

  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ket::Style::install(*qApp);
    }

    // Criterion 1: another team, another task of this team and a member's turn build nothing in
    // the task dialog and read no history.
    void theTaskDialogIgnoresChangesElsewhere() {
        start();
        QWidget window;
        window.resize(1100, 800);
        TeamPage page(teams_.get(), "7", "/work/shop", &window);
        page.resize(window.size());
        window.show();
        settle();
        ket::Dialog* dialog = page.openTask("T-1");
        settle();
        auto* body = dialog->findChild<TeamTaskDialog*>();
        QVERIFY(body);
        const int built = body->buildsForTesting();
        const int read = daemon_.count("team.task.get");
        QCOMPARE(read, 1);

        QJsonObject other = fixture();
        other.insert("id", "8");
        other.insert("name", "Billing");
        send(other);
        settle();
        send(withTask(fixture(), "T-2", [](QJsonObject& task) {
            task.insert("state", "in_review");
            task.insert("history", QJsonArray{entry("Created"), entry("Review")});
        }));
        settle();
        send(withActivity(withTask(fixture(), "T-2", [](QJsonObject& task) {
            task.insert("state", "in_review");
            task.insert("history", QJsonArray{entry("Created"), entry("Review")});
        })));
        settle();
        QCOMPARE(body->buildsForTesting(), built);
        QCOMPARE(daemon_.count("team.task.get"), read);
        dialog->reject();
    }

    // Criterion 2: a change of the task shows at once and reads its history; an answer that
    // repeats the history shown builds nothing more, and a new entry shows.
    void theTaskDialogFollowsItsTask() {
        start();
        QWidget window;
        window.resize(1100, 800);
        TeamPage page(teams_.get(), "7", "/work/shop", &window);
        page.resize(window.size());
        window.show();
        settle();
        ket::Dialog* dialog = page.openTask("T-1");
        settle();
        auto* body = dialog->findChild<TeamTaskDialog*>();
        QCOMPARE(dialog->findChildren<QLabel*>("teamTaskHistory").size(), 2);
        int built = body->buildsForTesting();
        int read = daemon_.count("team.task.get");

        const QString url = "https://example.org/projects/sample/pull/42";
        send(withTask(fixture(), "T-1", [&](QJsonObject& task) {
            task.insert("state", "in_review");
            task.insert("owner", "dev2");
            task.insert("pull_request", url);
            task.insert("criteria", QJsonArray{QJsonObject{{"text", "Works"}, {"met", true}},
                                               QJsonObject{{"text", "Tested"}, {"met", true}}});
            task.insert("criteria_met", 2);
        }));
        QCOMPARE(dialog->findChild<QLabel*>("teamTaskOwner")->text(), QStringLiteral("Dev 2"));
        QCOMPARE(dialog->findChild<QAbstractButton*>("teamTaskPullRequest")->toolTip(), url);
        QCOMPARE(dialog->findChild<QLabel*>("teamTaskCriteriaCount")->text(),
                 QStringLiteral("2 of 2 met"));
        // The same history came back: one build, for the task.
        QCOMPARE(daemon_.count("team.task.get"), read + 1);
        QCOMPARE(body->buildsForTesting(), built + 1);

        built = body->buildsForTesting();
        read = daemon_.count("team.task.get");
        daemon_.answers.insert("team.task.get", historyAnswer({entry("Created"), entry("Assigned"),
                                                               entry("In review")}));
        send(withTask(fixture(), "T-1", [&](QJsonObject& task) {
            task.insert("state", "in_review");
            task.insert("history", QJsonArray{entry("Created"), entry("In review")});
        }));
        QCOMPARE(daemon_.count("team.task.get"), read + 1);
        QCOMPARE(body->buildsForTesting(), built + 2);
        QCOMPARE(dialog->findChildren<QLabel*>("teamTaskHistory").size(), 3);
        // A member renamed is named again.
        QJsonObject renamed = withTask(fixture(), "T-1", [&](QJsonObject& task) {
            task.insert("state", "in_review");
            task.insert("history", QJsonArray{entry("Created"), entry("In review")});
        });
        QJsonArray members = renamed.value("members").toArray();
        QJsonObject dev1 = members.at(1).toObject();
        dev1.insert("name", "Dev One");
        members.replace(1, dev1);
        renamed.insert("members", members);
        read = daemon_.count("team.task.get");
        send(renamed);
        QCOMPARE(dialog->findChild<QLabel*>("teamTaskOwner")->text(), QStringLiteral("Dev One"));
        QCOMPARE(daemon_.count("team.task.get"), read);
        dialog->reject();
    }

    // Criterion 3: a burst of changes in one turn of the event loop builds once, and reads the
    // Updates once; another team's change reads nothing.
    void aBurstOfChangesBuildsOnce() {
        start();
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1000, 700);
        page.show();
        settle();
        const auto before = builds(page);
        const int reads = daemon_.count("team.get");
        for (int index = 0; index < 5; ++index)
            send(withTask(fixture(), "T-3", [index](QJsonObject& task) {
                task.insert("title", QStringLiteral("Title %1").arg(index));
            }));
        QCOMPARE(builds(page), before); // nothing yet: the page waits for the burst to end
        settle();
        QCOMPARE(page.sectionBuildsForTesting(Section::Tasks), before[Section::Tasks] + 1);
        QCOMPARE(page.tasksTable()->item(2, 1)->text(), QStringLiteral("Title 4"));
        QCOMPARE(daemon_.count("team.get"), reads + 1);

        const auto now = builds(page);
        QJsonObject other = fixture();
        other.insert("id", "8");
        send(other);
        settle();
        QCOMPARE(builds(page), now);
        QCOMPARE(daemon_.count("team.get"), reads + 1);
    }

    // Criterion 3: a member's activity leaves the task table, and a task's title the members.
    void aSectionBuildsOnlyForItsOwnData() {
        start();
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1000, 700);
        page.show();
        settle();
        auto before = builds(page);
        const QPointer<QTableWidget> table(page.tasksTable());
        send(withActivity(fixture()));
        settle();
        auto after = builds(page);
        QCOMPARE(after[Section::Members], before[Section::Members] + 1);
        QCOMPARE(after[Section::Tasks], before[Section::Tasks]);
        QCOMPARE(after[Section::Decisions], before[Section::Decisions]);
        QCOMPARE(after[Section::Heading], before[Section::Heading]);
        QCOMPARE(after[Section::Updates], before[Section::Updates]);
        QCOMPARE(page.tasksTable(), table.data());
        QCOMPARE(page.findChild<QWidget*>("teamMember_rev")
                     ->findChild<QLabel*>("teamMemberStatus")
                     ->text(),
                 QStringLiteral("Reviewing T-2"));

        before = after;
        const QPointer<QWidget> members(page.findChild<QWidget*>("teamMembersPanel"));
        send(withActivity(withTask(fixture(), "T-5", [](QJsonObject& task) {
            task.insert("title", "Renamed");
            task.insert("pull_request", "https://example.org/pull/5");
        })));
        settle();
        after = builds(page);
        QCOMPARE(after[Section::Tasks], before[Section::Tasks] + 1);
        QCOMPARE(after[Section::Members], before[Section::Members]);
        QCOMPARE(after[Section::Heading], before[Section::Heading]);
        QVERIFY(members);
        QCOMPARE(page.tasksTable()->item(4, 1)->text(), QStringLiteral("Renamed"));

        // The turns used show in the heading: it alone follows them.
        before = after;
        QJsonObject turns = withActivity(withTask(fixture(), "T-5", [](QJsonObject& task) {
            task.insert("title", "Renamed");
            task.insert("pull_request", "https://example.org/pull/5");
        }));
        turns.insert("automatic_turns", 24);
        send(turns);
        settle();
        after = builds(page);
        QCOMPARE(after[Section::Heading], before[Section::Heading] + 1);
        QCOMPARE(after[Section::Tasks], before[Section::Tasks]);
        QCOMPARE(after[Section::Members], before[Section::Members]);
        QCOMPARE(page.findChild<QLabel*>("teamPageTurns")->text(),
                 QStringLiteral("24 of 60 automatic turns used"));
    }

    // Criterion 4: the scroll position, the focus, the current task and an open task dialog
    // stay through changes.
    void aChangeKeepsTheReadersPlace() {
        start();
        QWidget window;
        window.resize(900, 360);
        TeamPage page(teams_.get(), "7", "/work/shop", &window);
        page.resize(window.size());
        window.show();
        settle();
        auto* bar = page.scrollArea()->verticalScrollBar();
        QVERIFY(bar->maximum() > 120);
        bar->setValue(120);
        // The focus in a section that is not built again stays on its very widget.
        const QPointer<QAbstractButton> open(page.findChild<QWidget*>("teamMember_dev2")
                                                 ->findChild<QAbstractButton*>("teamMemberOpen"));
        open->setFocus();
        send(withTask(fixture(), "T-4", [](QJsonObject& task) { task.insert("title", "Moved"); }));
        settle();
        QCOMPARE(bar->value(), 120);
        QVERIFY(open);
        QCOMPARE(window.focusWidget(), open.data());

        // The focus and the current task in the table come back after it is built again.
        page.tasksTable()->setCurrentCell(5, 0);
        page.tasksTable()->setFocus();
        const QPointer<QTableWidget> table(page.tasksTable());
        send(withTask(fixture(), "T-4", [](QJsonObject& task) { task.insert("title", "Again"); }));
        settle();
        QVERIFY(!table);
        QCOMPARE(page.tasksTable()->item(page.tasksTable()->currentRow(), 0)->text(),
                 QStringLiteral("T-6"));
        QCOMPARE(window.focusWidget(), static_cast<QWidget*>(page.tasksTable()));
        QCOMPARE(bar->value(), 120);

        // An open task dialog stays open through changes of the page under it.
        const QPointer<ket::Dialog> dialog(page.openTask("T-1"));
        settle();
        send(withActivity(
            withTask(fixture(), "T-4", [](QJsonObject& task) { task.insert("title", "Third"); })));
        settle();
        QVERIFY(dialog);
        QVERIFY(dialog->isVisible());
        dialog->reject();
    }

    // Criterion 4: the theme (light, dark, following the system) and every zoom step still
    // build the page and the open dialog at once.
    void theThemeAndTheZoomStillBuildEverything() {
        auto& appearance = ket::Appearance::instance();
        const auto oldMode = appearance.mode();
        const auto oldZoom = appearance.zoom();
        const auto restore = qScopeGuard([&] {
            appearance.setMode(oldMode);
            appearance.setZoom(oldZoom);
        });
        appearance.setMode(ket::Appearance::Mode::Light);
        appearance.setZoom(1.0);
        start();
        QWidget window;
        window.resize(1100, 800);
        TeamPage page(teams_.get(), "7", "/work/shop", &window);
        page.resize(window.size());
        window.show();
        settle();
        ket::Dialog* dialog = page.openTask("T-1");
        settle();
        auto* body = dialog->findChild<TeamTaskDialog*>();
        // A change of the theme or of the zoom builds every section and the dialog once.
        const auto rebuilds = [&](const std::function<void()>& change) {
            const auto before = builds(page);
            const int built = body->buildsForTesting();
            change();
            // At once, without waiting for the event loop.
            for (int section = 0; section < TeamPage::kSections; ++section)
                QCOMPARE(page.sectionBuildsForTesting(static_cast<Section>(section)),
                         before[static_cast<Section>(section)] + 1);
            QCOMPARE(body->buildsForTesting(), built + 1);
        };
        const auto light = appearance.theme();
        rebuilds([&] { appearance.setMode(ket::Appearance::Mode::Dark); });
        const auto dark = appearance.theme();
        QVERIFY(dark != light);
        // Following the system builds again when it shows the other theme than the one shown.
        appearance.setMode(ket::Appearance::Mode::System);
        settle();
        const auto system = appearance.theme();
        appearance.setMode(ket::Appearance::Mode::Dark);
        settle();
        if (system != dark)
            rebuilds([&] { appearance.setMode(ket::Appearance::Mode::System); });
        else
            appearance.setMode(ket::Appearance::Mode::System);
        appearance.setMode(ket::Appearance::Mode::Dark);
        rebuilds([&] { appearance.setMode(ket::Appearance::Mode::Light); });
        for (const qreal zoom : {1.25, 1.5, 1.0})
            rebuilds([&] { appearance.setZoom(zoom); });
        dialog->reject();
    }

    // Finding A3: building the page or the dialog makes no window. Before it shows, no widget of
    // it is created; shown, a rebuild shows nothing as a window and leaves no top-level widget.
    void buildingMakesNoWindow() {
        start();
        QJsonObject paused = fixture();
        paused.insert("paused", true);
        paused.insert("paused_reason", "user");
        send(paused);
        {
            TeamPage page(teams_.get(), "7", "/work/shop");
            TeamTaskDialog dialog(teams_.get(), "7", "T-1");
            QVERIFY(page.findChild<ket::Banner*>("teamPausedBanner"));
            for (QWidget* root : {static_cast<QWidget*>(&page), static_cast<QWidget*>(&dialog)})
                for (QWidget* widget : root->findChildren<QWidget*>())
                    QVERIFY2(!widget->testAttribute(Qt::WA_WState_Created),
                             qPrintable(QString::fromLatin1(widget->metaObject()->className()) +
                                        u' ' + widget->objectName()));
        }
        send(fixture()); // no banner from here on
        settle();
        QWidget window;
        window.resize(1100, 800);
        TeamPage page(teams_.get(), "7", "/work/shop", &window);
        page.resize(window.size());
        window.show();
        settle();
        ket::Dialog* dialog = page.openTask("T-1");
        settle();
        const auto topLevel = QApplication::topLevelWidgets().size();
        auto& appearance = ket::Appearance::instance();
        const auto oldZoom = appearance.zoom();
        const auto restore = qScopeGuard([&] { appearance.setZoom(oldZoom); });
        WindowWatch watch(&window);
        appearance.setZoom(1.25); // the whole page and the dialog
        settle();
        send(withTask(fixture(), "T-1", [](QJsonObject& task) { task.insert("title", "New"); }));
        settle();
        QCOMPARE(watch.shown, QStringList{});
        QCOMPARE(QApplication::topLevelWidgets().size(), topLevel);
        dialog->reject();
    }

    // Finding A3: team.changed in the directory's form of the team already read, and the
    // team.get answer that follows, change nothing.
    void theDirectoryFormOfAKnownTeamChangesNothing() {
        start();
        const QJsonObject full = [] {
            QJsonObject team = fixture();
            team.insert("revision", 40);
            return team;
        }();
        daemon_.answers.insert("team.get", {{"ok", true}, {"team", full}});
        teams_->applyEvent("team.changed", {{"team", full}});
        QSignalSpy changed(teams_.get(), &TeamDirectory::changed);
        QJsonObject directory = full;
        directory.remove("tasks");
        directory.remove("decisions");
        directory.insert("tasks_total", full.value("tasks").toArray().size());
        directory.insert("directory", true);
        teams_->applyEvent("team.changed", {{"team", directory}});
        teams_->request("team.get", {{"team_id", "7"}});
        QCOMPARE(changed.count(), 0);
        // A change in the directory's form still counts.
        directory.insert("revision", 41);
        directory.insert("name", "Shop 2");
        teams_->applyEvent("team.changed", {{"team", directory}});
        QCOMPARE(changed.count(), 1);
        QCOMPARE(teams_->team("7")->value("tasks").toArray().size(), 12);
    }
};

QTEST_MAIN(TeamPageRefreshTest)
#include "TeamPageRefreshTest.moc"
