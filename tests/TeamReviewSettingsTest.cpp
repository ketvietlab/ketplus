// T-40, T-53: a team's Max review passes per PR (one to five) and review place (the board or the
// pull request), chosen in New team and changed in the team's Review settings, against a fake
// daemon. Run with QT_QPA_PLATFORM=offscreen.
#include "teams/TeamReviewSettings.h"
#include "KetAppearance.h"
#include "KetBanner.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetProfile.h"
#include "KetSelect.h"
#include "KetStyle.h"
#include "KetTextField.h"
#include "KetToggle.h"
#include "teams/TeamDialogs.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPage.h"

#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QScrollArea>
#include <QStandardPaths>
#include <QtTest>

using namespace ketplus;

namespace {

QVariantMap model(const QString& provider, const QString& id) {
    return {
        {"provider", provider},
        {"model", id},
        {"displayName", id},
        {"isDefault", true},
        {"defaultReasoningEffort", "medium"},
        {"supportedReasoningEfforts", QVariantList{QVariantMap{{"reasoningEffort", "medium"}}}}};
}

const QVariantList models{model("codex", "gpt-5"), model("claude", "opus")};

QJsonObject team(const int rounds = 2, const QString& place = QStringLiteral("board")) {
    return {
        {"id", "7"},
        {"name", "Shop"},
        {"goal", "Checkout"},
        {"repository", "/work/shop"},
        {"integration_branch", "team/shop"},
        {"merge_policy", "leader"},
        {"turn_budget", 60},
        {"automatic_turns", 0},
        {"max_rounds", rounds},
        {"review_place", place},
        {"members",
         QJsonArray{QJsonObject{{"session_id", "lead"}, {"role", "leader"}, {"name", "Lead"}},
                    QJsonObject{{"session_id", "rev"}, {"role", "reviewer"}, {"name", "Review"}}}},
        {"tasks", QJsonArray{}},
        {"decisions", QJsonArray{}},
        {"revision", 1}};
}

void click(QWidget* root, const char* name) {
    auto* radio = root->findChild<ket::Radio*>(QLatin1String(name));
    QVERIFY2(radio, name);
    radio->click();
}

// Chooses `rounds` in the Max review passes select, as a click on its item does.
void choosePasses(QWidget* root, const int rounds) {
    auto* select = root->findChild<ket::Select*>(QStringLiteral("teamReviewRounds"));
    QVERIFY(select);
    QCOMPARE(select->count(), 5);
    select->setCurrentIndex(rounds - 1);
    emit select->activated(rounds - 1);
}

// The row's help, as its note label shows it.
QString help(QWidget* root, const char* row) {
    auto* field = root->findChild<ket::FieldRow*>(QLatin1String(row));
    if (!field)
        return {};
    QStringList texts;
    for (const QLabel* label : field->findChildren<QLabel*>(Qt::FindDirectChildrenOnly))
        texts.append(label->text());
    return texts.join(QLatin1Char('\n'));
}

} // namespace

class TeamReviewSettingsTest final : public QObject {
    Q_OBJECT

  private:
    QList<QPair<QString, QJsonObject>> sent_;
    QHash<QString, QJsonObject> answers_;
    std::unique_ptr<TeamDirectory> teams_;

    QJsonObject lastOf(const QString& type) const {
        for (auto it = sent_.crbegin(); it != sent_.crend(); ++it)
            if (it->first == type)
                return it->second;
        return {};
    }

  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ket::Style::install(*qApp);
        ket::Profile::setCurrent(ket::Profile::ketPlus());
    }

    void init() {
        sent_.clear();
        answers_.clear();
        answers_.insert("team.list", {{"ok", true}, {"teams", QJsonArray{team()}}});
        answers_.insert("team.get", {{"ok", true}, {"team", team()}});
        teams_ = std::make_unique<TeamDirectory>();
        teams_->setTransport([this](const QString& type, const QJsonObject& payload,
                                    TeamDirectory::Reply reply) {
            sent_.append({type, payload});
            const QJsonObject answer =
                answers_.value(type, {{"ok", true}, {"team", QJsonObject{{"id", "9"}}}});
            reply(answer, answer.value("ok").toBool() ? QString{}
                                                      : answer.value("error_message").toString());
        });
        teams_->setAvailable(true);
    }

    void theSummaryReadsTheTeam() {
        QCOMPARE(team_review::summary(QJsonObject{}),
                 QStringLiteral("2 review passes on the board"));
        QCOMPARE(team_review::summary(team(1, "pull_request")),
                 QStringLiteral("1 review pass on the pull request"));
        QCOMPARE(team_review::rounds(QJsonObject{{"max_rounds", 5}}), 5);
        // A value outside one to five reads as the standard two.
        QCOMPARE(team_review::rounds(QJsonObject{{"max_rounds", 6}}), 2);
        QCOMPARE(team_review::rounds(QJsonObject{{"max_rounds", 0}}), 2);
    }

    void theHelpFollowsTheChoice() {
        TeamReviewFields fields(2, "board");
        QVERIFY(help(&fields, "teamReviewRoundsRow").contains("Max review passes per PR"));
        QVERIFY(help(&fields, "teamReviewRoundsRow").contains("pass 2 go to the leader"));
        choosePasses(&fields, 4);
        QCOMPARE(fields.rounds(), 4);
        QVERIFY(help(&fields, "teamReviewRoundsRow").contains("until pass 4"));
        choosePasses(&fields, 1);
        QCOMPARE(fields.rounds(), 1);
        QVERIFY(help(&fields, "teamReviewRoundsRow").contains("only pass go to the leader"));
        click(&fields, "teamReviewPlacePullRequest");
        QCOMPARE(fields.place(), QStringLiteral("pull_request"));
        QVERIFY(help(&fields, "teamReviewPlaceRow")
                    .contains("A task without a pull request is reviewed on the board"));
    }

    void newTeamSendsTwoRoundsOnTheBoardUnlessChosen_data() {
        QTest::addColumn<bool>("choose");
        QTest::addColumn<int>("rounds");
        QTest::addColumn<QString>("place");
        QTest::newRow("standard") << false << 2 << "board";
        QTest::newRow("one") << true << 1 << "pull_request";
        QTest::newRow("five") << true << 5 << "pull_request";
    }
    void newTeamSendsTwoRoundsOnTheBoardUnlessChosen() {
        QFETCH(bool, choose);
        QFETCH(int, rounds);
        QFETCH(QString, place);
        QMainWindow window;
        window.resize(1024, 900);
        window.show();
        QPointer<ket::Dialog> dialog = NewTeamForm::present(&window, teams_.get(), "/repo", models);
        auto* form = dialog->findChild<NewTeamForm*>();
        form->nameField()->setText("Shop");
        if (choose) {
            choosePasses(form, rounds);
            click(form, "teamReviewPlacePullRequest");
        }
        const QJsonObject payload = form->payload();
        QCOMPARE(payload.value("max_rounds").toInt(), rounds);
        QCOMPARE(payload.value("review_place").toString(), place);
        dialog->reject();
    }

    void theTeamPageNamesTheReviewAndItsSettingsSendAnUpdate() {
        answers_.insert("team.get", {{"ok", true}, {"team", team(2, "board")}});
        TeamPage page(teams_.get(), "7", "/work/shop");
        page.resize(1280, 900);
        page.show();
        QTRY_VERIFY(page.findChild<QLabel*>("teamPageReview"));
        QCOMPARE(page.findChild<QLabel*>("teamPageReview")->text(),
                 QStringLiteral("2 review passes on the board"));
        QPointer<ket::Dialog> dialog = teamReviewSettings(&page, teams_.get(), team(2, "board"));
        auto* fields = dialog->findChild<TeamReviewFields*>();
        QCOMPARE(fields->rounds(), 2);
        QCOMPARE(fields->place(), QStringLiteral("board"));
        QVERIFY(
            help(dialog, "teamReviewPlaceRow").contains("Applies from the next review request"));
        // Nothing changed: Save closes without a request.
        dialog->findChild<ket::Button*>("teamReviewSave")->click();
        QTRY_VERIFY(!dialog);
        QVERIFY(lastOf("team.update").isEmpty());

        // The daemon's refusal stays in the dialog, in its words.
        answers_.insert("team.update",
                        {{"ok", false},
                         {"error_code", "invalid_payload"},
                         {"error_message", "A team reviews a task in one to five passes before "
                                           "the leader decides."}});
        dialog = teamReviewSettings(&page, teams_.get(), team(2, "board"));
        fields = dialog->findChild<TeamReviewFields*>();
        choosePasses(fields, 3);
        click(fields, "teamReviewPlacePullRequest");
        dialog->findChild<ket::Button*>("teamReviewSave")->click();
        const QJsonObject update = lastOf("team.update");
        QCOMPARE(update.value("team_id").toString(), QStringLiteral("7"));
        QCOMPARE(update.value("max_rounds").toInt(), 3);
        QCOMPARE(update.value("review_place").toString(), QStringLiteral("pull_request"));
        auto* refusal = dialog->findChild<ket::Banner*>("teamReviewRefusal");
        QTRY_VERIFY(!refusal->isHidden());
        QVERIFY(refusal->message().contains("one to five passes"));
        QVERIFY(dialog->isVisible());

        // Taken, the dialog closes.
        answers_.remove("team.update");
        dialog->findChild<ket::Button*>("teamReviewSave")->click();
        QTRY_VERIFY(!dialog);
    }

    // With KETPLUS_TEAM_REVIEW_CAPTURE set to a folder, saves New team at its review rows and
    // the team's Review settings, in light and dark, beside the mock's `--open=new-team` and
    // `--open=team-review`.
    void captureForReview() {
        const QString folder = qEnvironmentVariable("KETPLUS_TEAM_REVIEW_CAPTURE");
        if (folder.isEmpty())
            QSKIP("KETPLUS_TEAM_REVIEW_CAPTURE is not set.");
        auto& appearance = ket::Appearance::instance();
        for (const auto& [mode, suffix] : {std::pair(ket::Appearance::Mode::Light, "light"),
                                           std::pair(ket::Appearance::Mode::Dark, "dark")}) {
            appearance.setMode(mode);
            TeamPage page(teams_.get(), "7", "/work/shop");
            page.resize(1280, 900);
            page.show();
            QTRY_VERIFY(page.findChild<QLabel*>("teamPageReview"));
            QPointer<ket::Dialog> create =
                NewTeamForm::present(&page, teams_.get(), "/work/shop", models);
            auto* form = create->findChild<NewTeamForm*>();
            if (auto* scroll = create->findChild<QScrollArea*>())
                scroll->ensureWidgetVisible(form->findChild<TeamReviewFields*>(), 0, 120);
            QTest::qWait(250);
            QVERIFY(page.grab().save(QDir(folder).filePath(
                QStringLiteral("app-new-team-%1.png").arg(QLatin1String(suffix)))));
            create->reject();
            delete create.data();
            QPointer<ket::Dialog> review = teamReviewSettings(&page, teams_.get(), team());
            QTest::qWait(250);
            QVERIFY(page.grab().save(QDir(folder).filePath(
                QStringLiteral("app-team-review-%1.png").arg(QLatin1String(suffix)))));
            review->reject();
            delete review.data();
        }
        appearance.setMode(ket::Appearance::Mode::Light);
    }
};

QTEST_MAIN(TeamReviewSettingsTest)
#include "TeamReviewSettingsTest.moc"
