#include "teams/TeamDecisionView.h"
#include "KetAppearance.h"
#include "KetBadge.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetStyle.h"
#include "TeamLanguage.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPage.h"
#include "teams/TeamTaskDialog.h"

#include <QAbstractButton>
#include <QDir>
#include <QJsonArray>
#include <QLabel>
#include <QLocale>
#include <QScopeGuard>
#include <QScrollArea>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QtTest>

using namespace ketplus;
namespace {
QJsonObject decision(const QString& route = "developer_reviewer") {
    return {{"version", 1},
            {"route", route},
            {"actor", "lead"},
            {"risk", route == "leader_direct" ? "advisory" : "high_or_unknown"},
            {"gate_reason", "risk_unknown"},
            {"reason", "conservative_review"},
            {"revision", QString(40, 'a')},
            {"evidence_ids", QJsonArray{QString(64, 'b')}},
            {"follow_up", route == "leader_direct" ? "answer_only" : "review_required"}};
}
QJsonObject task() {
    return {{"id", "T-1"},
            {"title", "Synthetic decision fixture"},
            {"owner", "dev"},
            {"state", "blocked"},
            {"round", 1},
            {"round_limit", 2},
            {"head_commit", QString(40, 'a')},
            {"routing_decision", decision()},
            {"review_block", "review_interrupted"}};
}
QJsonObject team() {
    return {
        {"id", "7"},
        {"name", "Synthetic team"},
        {"language", "en"},
        {"revision", 1},
        {"members",
         QJsonArray{
             QJsonObject{{"session_id", "lead"}, {"role", "leader"}, {"name", "Leader Ada"}},
             QJsonObject{{"session_id", "dev"}, {"role", "developer"}, {"name", "Developer Lin"}}}},
        {"tasks", QJsonArray{task()}}};
}
QString label(QWidget& root, const char* name) {
    const auto* found = root.findChild<QLabel*>(QLatin1String(name));
    return found ? found->text() : QString{};
}

bool readOnlyRequest(const QString& call) {
    // TeamPage also reads its existing Updates/storage sections. Keep a concrete read-only
    // allowlist so opening a decision cannot quietly add a route/acceptance mutation.
    return call == "team.list" || call == "team.get" || call == "team.task.get" ||
           call == "team.updates.list" || call == "team.storage.get";
}
QJsonObject finding(const QString& severity, const QString& status = "open") {
    return {{"id", "required_check"},
            {"severity", severity},
            {"impact", "required_check"},
            {"scope", "task"},
            {"status", status},
            {"mandatory_rule", true},
            {"close_when", "required_matrix_passes"},
            {"evidence", QString(64, 'e')}};
}
} // namespace

class TeamDecisionViewTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ket::Style::install(*qApp);
        const QString theme = qEnvironmentVariable("KETPLUS_TEAM_DECISION_THEME");
        if (theme == "light")
            ket::Appearance::instance().setMode(ket::Appearance::Mode::Light);
        else if (theme == "dark")
            ket::Appearance::instance().setMode(ket::Appearance::Mode::Dark);
    }
    void allRoutesAreHistoricalAndHaveNoMutationControls_data() {
        QTest::addColumn<QString>("route");
        QTest::addColumn<QString>("name");
        QTest::newRow("direct") << QStringLiteral("leader_direct")
                                << QStringLiteral("Leader answers directly");
        QTest::newRow("leader") << QStringLiteral("developer_leader")
                                << QStringLiteral("Developer → leader");
        QTest::newRow("reviewer") << QStringLiteral("developer_reviewer")
                                  << QStringLiteral("Developer → reviewer");
    }
    void allRoutesAreHistoricalAndHaveNoMutationControls() {
        QFETCH(QString, route);
        QFETCH(QString, name);
        remote::team_language::Scope language("en");
        auto t = task();
        t.insert("routing_decision", decision(route));
        TeamDecisionView view(team(), t);
        QCOMPARE(view.findChild<ket::Badge*>("teamDecisionRoute")->text(), name);
        QVERIFY(label(view, "teamDecisionActor").contains("Leader Ada"));
        QVERIFY(label(view, "teamDecisionHistorical")
                    .contains("not a reviewer verdict or user approval"));
        QVERIFY(label(view, "teamDecisionChecks").contains("not included"));
        QVERIFY(view.findChildren<QAbstractButton*>().isEmpty());
    }
    void missingUnknownAndChangedRecordsDoNotImplyPass() {
        QVERIFY(!TeamDecisionView::hasDetails({}));
        auto t = task();
        auto d = decision();
        d.insert("version", 99);
        t.insert("routing_decision", d);
        TeamDecisionView unknown(team(), t);
        QVERIFY(unknown.findChild<QLabel*>("teamDecisionUnavailable"));
        QVERIFY(!unknown.findChild<QWidget*>("teamDecisionRoute"));
        t.insert("routing_decision", decision("developer_leader"));
        t.insert("head_commit", QString(40, 'c'));
        TeamDecisionView changed(team(), t);
        QVERIFY(label(changed, "teamDecisionChangedHead").contains("does not verify"));
        QVERIFY(!changed.findChildren<QLabel*>("teamDecisionEvidenceReference").isEmpty());
    }
    void interruptedReviewNeverAccusesReviewerOrConsumesContentRound() {
        const QJsonObject event{
            {"kind", "review_interrupted"},
            {"text", "Reviewer changed branch — stale legacy message"},
            {"data", QJsonObject{{"round", 2}, {"content_round_counted", false}}}};
        const QString text = TeamDecisionView::eventText(event);
        QVERIFY(text.contains("Round 2"));
        QVERIFY(text.contains("No content round counted"));
        QVERIFY(!text.contains("changed branch"));
        QVERIFY(!text.contains("source unavailable")); // A failed review turn need not lose source.
        auto tampered = event;
        tampered.insert("kind", "review_tampered");
        QVERIFY(TeamDecisionView::eventText(tampered).contains("source changed"));
        TeamDecisionView view(team(), task());
        QCOMPARE(label(view, "teamDecisionRounds"), QStringLiteral("Content review round: 1 of 2"));
    }
    void findingsKeepMandatoryUnknownAndLegacyBlocking() {
        auto t = task();
        QJsonObject record{
            {"version", 1},
            {"legacy", false},
            {"reviewer", "review"},
            {"findings", QJsonArray{finding("p1", "fixed"), finding("unknown"), finding("p2")}}};
        t.insert("review_findings", record);
        TeamDecisionView view(team(), t);
        const auto rows = view.findChildren<QLabel*>("teamDecisionFinding");
        QCOMPARE(rows.size(), 3);
        for (const auto* row : rows)
            QVERIFY(row->text().contains("review required"));
        QCOMPARE(view.findChildren<QLabel*>("teamDecisionFindingMandatory").size(), 3);
        record.insert("legacy", true);
        t.insert("review_findings", record);
        TeamDecisionView legacy(team(), t);
        QVERIFY(label(legacy, "teamDecisionLegacyFindings").contains("closure is not inferred"));
    }
    void pairedActorAndLocalActorHaveTheSameMeaning() {
        QCOMPARE(TeamDecisionView::actorName(team(), "lead"), QStringLiteral("Leader Ada"));
        QCOMPARE(
            TeamDecisionView::actorName(team(), QJsonObject{{"role", "leader"}, {"number", "42"}}),
            QStringLiteral("Leader #42"));
        QCOMPARE(TeamDecisionView::actorName(team(), QJsonObject{{"role", "daemon"}}),
                 QStringLiteral("ketPlus"));
    }
    void queueWaitNeedsACompleteOrderedPairForTheCurrentRound() {
        auto t = task();
        const QJsonObject queued{{"kind", "state"},
                                 {"at", "2026-11-17T10:00:00.000Z"},
                                 {"data", QJsonObject{{"to", "in_review"}}}};
        const QJsonObject started{{"kind", "review"},
                                  {"at", "2026-11-17T10:00:12.000Z"},
                                  {"data", QJsonObject{{"round", 1}}}};
        t.insert("history", QJsonArray{queued, started});
        TeamDecisionView complete(team(), t);
        QCOMPARE(label(complete, "teamDecisionQueueWait"),
                 QStringLiteral("Last recorded review queue wait: 12 s"));
        t.insert("history", QJsonArray{started});
        TeamDecisionView partial(team(), t);
        QVERIFY(!partial.findChild<QLabel*>("teamDecisionQueueWait"));
        t.insert("history", QJsonArray{started, queued});
        TeamDecisionView reversed(team(), t);
        QVERIFY(!reversed.findChild<QLabel*>("teamDecisionQueueWait"));
    }
    void sourceUnavailableHasTheSameLocalAndPairedPresentation() {
        QJsonObject local{
            {"kind", "review_interrupted"},
            {"data", QJsonObject{{"round", 1},
                                 {"changes", QJsonArray{QJsonObject{{"field", "worktree"},
                                                                    {"before", "unreadable"}}}}}}};
        const QJsonObject paired{{"kind", "review_interrupted"},
                                 {"data", QJsonObject{{"round", 1}, {"source_unavailable", true}}}};
        QCOMPARE(TeamDecisionView::eventText(local), TeamDecisionView::eventText(paired));
        QVERIFY(TeamDecisionView::eventText(local).contains("source unavailable"));
        QVERIFY(!TeamDecisionView::eventText(local).contains("source changed"));
    }
    void renderedTranslationsAndLongReferencesFit_data() {
        QTest::addColumn<QString>("language");
        QTest::addColumn<QString>("locale");
        QTest::addColumn<int>("width");
        for (const auto& language : {QStringLiteral("en"), QStringLiteral("vi")})
            for (const auto& locale : {QStringLiteral("en_US"), QStringLiteral("vi_VN")})
                for (const int width : {900, 360})
                    QTest::newRow(
                        qPrintable(QStringLiteral("%1-%2-%3").arg(language, locale).arg(width)))
                        << language << locale << width;
    }
    void renderedTranslationsAndLongReferencesFit() {
        QFETCH(QString, language);
        QFETCH(QString, locale);
        const auto originalLocale = QLocale();
        const auto restoreLocale = qScopeGuard([&] { QLocale::setDefault(originalLocale); });
        QLocale::setDefault(QLocale(locale));
        QFETCH(int, width);
        remote::team_language::Scope scope(language);
        auto t = task();
        auto d = decision();
        d.insert("reason", QString(99, 'r'));
        t.insert("routing_decision", d);
        QScrollArea window;
        window.setWidgetResizable(true);
        auto* view = new TeamDecisionView(team(), t);
        window.setWidget(view);
        window.resize(width, 650);
        window.show();
        QTest::qWait(50);
        QCOMPARE(label(*view, "teamDecisionTrailHeading"), QStringLiteral("Decision and review"));
        QVERIFY(label(*view, "teamDecisionHistorical").contains("not a reviewer verdict"));
        for (auto* text : view->findChildren<QLabel*>()) {
            QVERIFY2(text->geometry().right() < view->width(), qPrintable(text->objectName()));
            if (text->hasHeightForWidth())
                QVERIFY2(text->height() >= text->heightForWidth(text->width()),
                         qPrintable(text->objectName()));
        }
        // Extra viewport height must not spread the read-only metadata lines apart.
        auto* actor = view->findChild<QLabel*>("teamDecisionActor");
        QVERIFY(actor);
        const int actorY = actor->y();
        window.resize(width, 1000);
        QTest::qWait(20);
        QCOMPARE(actor->y(), actorY);
        window.resize(width, 650);
        QTest::qWait(20);
        const QString capture = qEnvironmentVariable("KETPLUS_TEAM_DECISION_CAPTURE");
        if (!capture.isEmpty()) {
            QVERIFY(QDir().mkpath(capture));
            QVERIFY(window.grab().save(
                capture +
                QStringLiteral("/decision-%1-%2-%3.png").arg(language, locale).arg(width)));
        }
    }
    void livePageRefreshAndTaskOpenOnlyReadDaemonData() {
        auto current = team();
        QStringList calls;
        TeamDirectory teams;
        teams.setTransport([&](const QString& type, const QJsonObject&,
                               TeamDirectory::Reply reply) {
            calls.append(type);
            if (type == "team.list")
                reply({{"ok", true}, {"teams", QJsonArray{current}}}, {});
            else if (type == "team.get")
                reply({{"ok", true}, {"team", current}, {"updates", QJsonArray{}}}, {});
            else
                reply({{"ok", true}, {"task", current.value("tasks").toArray().first().toObject()}},
                      {});
        });
        teams.setAvailable(true);
        TeamPage page(&teams, "7", {});
        page.resize(850, 650);
        page.show();
        QTRY_VERIFY(page.findChild<ket::Button*>("teamDecisionTaskOpen"));
        QVERIFY(label(page, "teamDecisionTrailHistorical").contains("Historical records"));
        QTest::mouseClick(page.findChild<ket::Button*>("teamDecisionTaskOpen"), Qt::LeftButton);
        QTRY_VERIFY(page.findChild<TeamTaskDialog*>());
        auto changedTask = task();
        auto d = decision("developer_leader");
        changedTask.insert("routing_decision", d);
        current.insert("tasks", QJsonArray{changedTask});
        current.insert("revision", 2);
        teams.applyEvent("team.changed", {{"team", current}});
        QTRY_VERIFY(label(page, "teamDecisionTaskSummary").contains("Developer → leader"));
        QVERIFY(!calls.contains("team.route.decide"));
        QVERIFY(!calls.contains("team.review.accept"));
        for (const auto& call : calls)
            QVERIFY2(readOnlyRequest(call), qPrintable(calls.join(QStringLiteral(", "))));
    }
    void pairedDetailsCannotOverwriteARefreshedTask() {
        auto current = team();
        auto summary = task();
        summary.insert("changed_revision", 1);
        summary.insert("review_findings", QJsonObject{{"version", 1},
                                                      {"legacy", false},
                                                      {"summary_only", true},
                                                      {"finding_total", 1}});
        current.insert("tasks", QJsonArray{summary});
        QList<TeamDirectory::Reply> pending;
        TeamDirectory teams;
        teams.setTransport(
            [&](const QString& type, const QJsonObject&, TeamDirectory::Reply reply) {
                if (type == "team.list")
                    reply({{"ok", true}, {"teams", QJsonArray{current}}}, {});
                else if (type == "team.task.get")
                    pending.append(std::move(reply));
                else
                    reply({{"ok", true}}, {});
            });
        teams.setAvailable(true);
        TeamTaskDialog body(&teams, "7", "T-1");
        QCOMPARE(pending.size(), 1);
        QVERIFY(!label(body, "teamDecisionFindingsSummary").isEmpty());
        auto detail = summary;
        detail.insert("review_findings", QJsonObject{{"version", 1},
                                                     {"legacy", false},
                                                     {"reviewer", "lead"},
                                                     {"findings", QJsonArray{finding("p2")}}});
        pending.takeFirst()({{"task", detail}}, {});
        QVERIFY(!label(body, "teamDecisionFinding").isEmpty());
        QVERIFY(label(body, "teamDecisionFindingsSummary").isEmpty());

        summary.insert("changed_revision", 2);
        current.insert("tasks", QJsonArray{summary});
        current.insert("revision", 2);
        teams.applyEvent("team.changed", {{"team", current}});
        QCOMPARE(pending.size(), 1);
        QVERIFY(label(body, "teamDecisionFinding").isEmpty());
        summary.insert("changed_revision", 3);
        summary.insert("head_commit", QString(40, 'c'));
        current.insert("tasks", QJsonArray{summary});
        current.insert("revision", 3);
        teams.applyEvent("team.changed", {{"team", current}});
        pending.takeFirst()({{"task", detail}}, {}); // stale reply triggers a fresh read
        QCOMPARE(pending.size(), 1);
        QVERIFY(label(body, "teamDecisionFinding").isEmpty());
        detail = summary;
        detail.insert("review_findings", QJsonObject{{"version", 1},
                                                     {"legacy", false},
                                                     {"reviewer", "lead"},
                                                     {"findings", QJsonArray{finding("p1")}}});
        pending.takeFirst()({{"task", detail}}, {});
        QVERIFY(label(body, "teamDecisionFinding").contains("P1"));
    }

    void taskShowsAssignedReviewerAcrossTwoReviewerSlots() {
        auto current = team();
        auto members = current.value("members").toArray();
        members.append(
            QJsonObject{{"session_id", "rev1"}, {"role", "reviewer"}, {"name", "Review One"}});
        members.append(
            QJsonObject{{"session_id", "rev2"}, {"role", "reviewer"}, {"name", "Review Two"}});
        current.insert("members", members);
        auto t = task();
        t.insert("reviewer", "rev1");
        current.insert("tasks", QJsonArray{t});
        TeamDirectory teams;
        teams.setTransport(
            [&](const QString& type, const QJsonObject&, TeamDirectory::Reply reply) {
                if (type == "team.list")
                    reply({{"ok", true}, {"teams", QJsonArray{current}}}, {});
                else
                    reply({{"ok", true}}, {});
            });
        teams.setAvailable(true);
        TeamTaskDialog body(&teams, "7", "T-1");
        QCOMPARE(label(body, "teamTaskReviewer"), QStringLiteral("Review One"));
        t.insert("reviewer", "rev2");
        current.insert("tasks", QJsonArray{t});
        current.insert("revision", 2);
        teams.applyEvent("team.changed", {{"team", current}});
        QCOMPARE(label(body, "teamTaskReviewer"), QStringLiteral("Review Two"));
    }
    void composedPageAndTaskStayKeyboardAccessible_data() {
        QTest::addColumn<QString>("language");
        QTest::addColumn<QString>("locale");
        QTest::addColumn<int>("width");
        for (const auto& language : {QStringLiteral("en"), QStringLiteral("vi")})
            for (const auto& locale : {QStringLiteral("en_US"), QStringLiteral("vi_VN")})
                for (const int width : {1000, 420})
                    QTest::newRow(
                        qPrintable(QStringLiteral("%1-%2-%3").arg(language, locale).arg(width)))
                        << language << locale << width;
    }
    void composedPageAndTaskStayKeyboardAccessible() {
        QFETCH(QString, language);
        QFETCH(QString, locale);
        const auto originalLocale = QLocale();
        const auto restoreLocale = qScopeGuard([&] { QLocale::setDefault(originalLocale); });
        QLocale::setDefault(QLocale(locale));
        QFETCH(int, width);
        auto current = team();
        current.insert("language", language);
        const QString contentName = language == "vi" ? QStringLiteral("Nhóm thử nghiệm tổng hợp")
                                                     : QStringLiteral("Synthetic audit team");
        current.insert("name", contentName);
        current.insert("goal", language == "vi"
                                   ? QStringLiteral("Kiểm chứng quyết định bằng dữ liệu giả")
                                   : QStringLiteral("Verify decisions with synthetic content"));
        auto members = current.value("members").toArray();
        auto leader = members.first().toObject();
        leader.insert("name", contentName);
        members.replace(0, leader);
        current.insert("members", members);
        QStringList calls;
        TeamDirectory teams;
        teams.setTransport(
            [&](const QString& type, const QJsonObject&, TeamDirectory::Reply reply) {
                calls.append(type);
                if (type == "team.list")
                    reply({{"ok", true}, {"teams", QJsonArray{current}}}, {});
                else if (type == "team.get")
                    reply({{"ok", true}, {"team", current}, {"updates", QJsonArray{}}}, {});
                else
                    reply({{"ok", true}, {"task", current.value("tasks").toArray().first()}}, {});
            });
        teams.setAvailable(true);
        TeamPage page(&teams, "7", {});
        page.resize(width, 720);
        page.show();
        auto* historical = page.findChild<QLabel*>("teamDecisionTrailHistorical");
        QTRY_VERIFY(historical && historical->isVisible());
        QVERIFY2(historical->contentsRect().height() >=
                     historical->heightForWidth(historical->contentsRect().width()),
                 "The historical-record warning is clipped by its content margins");
        auto* open = page.findChild<ket::Button*>("teamDecisionTaskOpen");
        QTRY_VERIFY(open && open->isVisible());
        open->setFocus();
        QTRY_VERIFY(open->hasFocus());
        const QString capture = qEnvironmentVariable("KETPLUS_TEAM_DECISION_CAPTURE");
        if (!capture.isEmpty()) {
            QVERIFY(QDir().mkpath(capture));
            QVERIFY(page.grab().save(
                capture + QStringLiteral("/page-%1-%2-%3.png").arg(language, locale).arg(width)));
        }
        QTest::keyClick(open, Qt::Key_Space);
        QTRY_VERIFY(page.findChild<TeamTaskDialog*>());
        auto* body = page.findChild<TeamTaskDialog*>();
        QVERIFY(body->findChild<TeamDecisionView*>());
        const auto* trail = body->findChild<TeamDecisionView*>();
        const QString heading = label(*body, "teamDecisionTrailHeading");
        QVERIFY(!heading.isEmpty());
        QCOMPARE(heading, QStringLiteral("Decision and review"));
        QCOMPARE(body->findChild<ket::Badge*>("teamTaskState")->text(), QStringLiteral("Blocked"));
        QVERIFY(label(*body, "teamDecisionActor").contains(contentName));
        QCOMPARE(label(page, "teamPageName"), contentName);
        for (auto* text : trail->findChildren<QLabel*>())
            QVERIFY2(text->geometry().right() < trail->width(), qPrintable(text->objectName()));
        if (!capture.isEmpty())
            QVERIFY(page.grab().save(
                capture + QStringLiteral("/task-%1-%2-%3.png").arg(language, locale).arg(width)));
        for (const auto& call : calls)
            QVERIFY2(readOnlyRequest(call), qPrintable(calls.join(QStringLiteral(", "))));
    }
    void liveThemeAndZoomKeepPageAndTaskTrailAccessible() {
        auto& appearance = ket::Appearance::instance();
        const auto oldMode = appearance.mode();
        const auto oldZoom = appearance.zoom();
        const auto restore = qScopeGuard([&] {
            appearance.setMode(oldMode);
            appearance.setZoom(oldZoom);
        });
        const auto current = team();
        TeamDirectory teams;
        teams.setTransport(
            [&](const QString& type, const QJsonObject&, TeamDirectory::Reply reply) {
                if (type == "team.list")
                    reply({{"ok", true}, {"teams", QJsonArray{current}}}, {});
                else if (type == "team.get")
                    reply({{"ok", true}, {"team", current}, {"updates", QJsonArray{}}}, {});
                else
                    reply({{"ok", true}}, {});
            });
        teams.setAvailable(true);
        TeamPage page(&teams, "7", {});
        page.resize(1000, 800);
        page.show();
        auto* dialog = page.openTask("T-1");
        QVERIFY(dialog);
        for (const auto mode : {ket::Appearance::Mode::Light, ket::Appearance::Mode::Dark,
                                ket::Appearance::Mode::System}) {
            appearance.setMode(mode);
            for (const qreal zoom : {0.9, 1.0, 1.1, 1.25}) {
                appearance.setZoom(zoom);
                QTest::qWait(20);
                auto* body = dialog->findChild<TeamTaskDialog*>();
                QVERIFY(body);
                auto* trail = body->findChild<TeamDecisionView*>();
                QVERIFY(trail);
                for (auto* text : trail->findChildren<QLabel*>())
                    QVERIFY2(text->geometry().right() < trail->width(),
                             qPrintable(text->objectName()));
                QVERIFY(page.findChild<ket::Button*>("teamDecisionTaskOpen"));
            }
        }
    }
};
QTEST_MAIN(TeamDecisionViewTest)
#include "TeamDecisionViewTest.moc"
