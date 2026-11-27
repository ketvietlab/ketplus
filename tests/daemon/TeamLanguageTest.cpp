// T30: content preferences survive while new fixed system text stays English.
// A fake runtime; no provider runs.
#include "TeamLanguage.h"
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "agents/AgentInstructions.h"
#include "storage/LocalJournal.h"
#include "teams/TeamRules.h"

#include <QSettings>
#include <QStandardPaths>
#include <QTest>

using namespace ketplus::daemon;
using namespace team_test;

class TeamLanguageTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    void open() {
        journal_ = std::make_unique<LocalJournal>(directory_.filePath("journal.sqlite3"));
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->setQuietPeriod(60 * 60 * 1000);
        teams_->load();
    }
    void start(const QString& language, const int budget = 40) {
        Team draft = standardTeam();
        draft.language = language;
        draft.turnBudget = budget;
        team_ = teams_->create(draft, "user").teamId;
        QVERIFY(team_ > 0);
    }
    const Team& team() { return *teams_->team(team_); }
    QString task(const QString& title) {
        QString id;
        (void)teams_->createTask(team_, {.title = title, .lane = "Web"}, "lead", &id);
        return id;
    }
    // Newest first, as the journal answers.
    QList<TeamUpdateRecord> updates(const QString& kind) {
        QList<TeamUpdateRecord> found;
        for (const TeamUpdateRecord& update : journal_->teamUpdates(team_, false, 300))
            if (update.kind == kind)
                found.append(update);
        return found;
    }
    // Two automatic turns of a budget of 2: the third assignment pauses the team and asks.
    const Decision& useTheBudget(const QString& language) {
        start(language, 2);
        const QStringList ids{task("One"), task("Two"), task("Three")};
        (void)teams_->assignTask(team_, ids.at(0), "dev1", "lead");
        (void)teams_->assignTask(team_, ids.at(1), "dev2", "lead");
        (void)teams_->assignTask(team_, ids.at(2), "dev1", "lead");
        return team().decisions.last();
    }

  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(directory_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory_.path());
    }

    void init() {
        teams_.reset();
        journal_.reset();
        QFile::remove(directory_.filePath("journal.sqlite3"));
        host_ = {};
        for (const QString& id : {"lead", "dev1", "dev2"})
            host_.add(id);
        host_.add("rev", "claude");
        open();
    }

    void theLanguageNamesAreTheSupportedOnes() {
        namespace language = ketplus::remote::team_language;
        QCOMPARE(language::supported(), (QStringList{"auto", "en", "vi"}));
        QCOMPARE(language::fromLocale("vi-VN"), QStringLiteral("vi"));
        QCOMPARE(language::fromLocale("vi_VN"), QStringLiteral("vi"));
        QCOMPARE(language::fromLocale("fr-FR"), QStringLiteral("en"));
        QCOMPARE(language::orEnglish({}), QStringLiteral("en"));
        QCOMPARE(language::detect("Thanh toán bằng Adyen"), QStringLiteral("vi"));
        QCOMPARE(language::detect("Pay with Adyen"), QString{});
        QVERIFY(!teams::rules::validLanguage("vi"));
        QVERIFY(!teams::rules::validLanguage("auto"));
        QCOMPARE(Team{}.language, QStringLiteral("auto"));
        const auto refused = teams::rules::validLanguage("fr");
        QVERIFY(refused);
        QCOMPARE(refused->code, QStringLiteral("invalid_payload"));
    }

    void fixedFormattingIgnoresAllContentAndAmbientPreferences() {
        namespace language = ketplus::remote::team_language;
        QObject owner;
        for (const QString& preference : language::supported()) {
            language::setAmbient(preference, &owner);
            language::Scope scope(preference);
            language::install();
            QCOMPARE(language::current(), QString("en"));
            QCOMPARE(QCoreApplication::translate("Team", "Members"), QString("Members"));
            QCOMPARE(language::text(preference, "Team", "%n tasks", 3), QString("3 tasks"));
            language::clearAmbient(&owner);
        }
    }

    void historicalContentAndDecisionChoicesSurvivePreferenceChangeAndRestart() {
        start("vi");
        const QString id = task("Giữ nguyên tiêu đề");
        QString decision;
        QVERIFY(teams_
                    ->raiseQuestion(team_,
                                    {{"question", "Dùng cách cũ?"},
                                     {"choices", QJsonArray{"Tiếp tục", "Dừng"}},
                                     {"context", "Không dịch lại lịch sử"}},
                                    "lead", &decision)
                    .ok());
        QVERIFY(teams_->update(team_, {{"language", "auto"}}, "user").ok());
        teams_.reset();
        open();
        QCOMPARE(team().language, QString("auto"));
        QCOMPARE(team().task(id)->title, QString("Giữ nguyên tiêu đề"));
        QCOMPARE(team().decisions.last().question, QString("Dùng cách cũ?"));
        QCOMPARE(team().decisions.last().choices, (QStringList{"Tiếp tục", "Dừng"}));
        QVERIFY(teams_->answerDecision(team_, decision, "Tiếp tục", "user").ok());
        QCOMPARE(team().decisions.last().answer, QString("Tiếp tục"));
    }

    // Stored with the team, read back after a restart, changed in settings and logged.
    void theLanguageIsStoredAndAChangeIsLogged() {
        start("vi");
        QCOMPARE(team().language, QStringLiteral("vi"));
        QCOMPARE(updates("team_created").first().text,
                 QStringLiteral("Team Checkout created with 4 members."));
        teams_.reset();
        open();
        QCOMPARE(team().language, QStringLiteral("vi"));

        const auto refused = teams_->update(team_, {{"language", "fr"}}, "user");
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error->code, QStringLiteral("invalid_payload"));
        QCOMPARE(team().language, QStringLiteral("vi"));

        QVERIFY(teams_->update(team_, {{"language", "en"}}, "user").ok());
        QCOMPARE(team().language, QStringLiteral("en"));
        const auto changed = updates("language");
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.first().text, QStringLiteral("Content language preference: English."));
        QCOMPARE(changed.first().data.value("from").toString(), QStringLiteral("vi"));
        QCOMPARE(changed.first().data.value("to").toString(), QStringLiteral("en"));
        // The same language again changes nothing and logs nothing.
        QVERIFY(teams_->update(team_, {{"language", "en"}}, "user").ok());
        QCOMPARE(updates("language").size(), 1);
        QVERIFY(teams_->update(team_, {{"language", "vi"}}, "user").ok());
        QCOMPARE(updates("language").first().text,
                 QStringLiteral("Content language preference: Vietnamese."));
    }

    // Missing preference follows the prompt; explicit legacy values remain unchanged.
    void aTeamWithoutALanguageFollowsThePrompt() {
        QJsonObject saved = toJson(standardTeam());
        saved.remove("language");
        QCOMPARE(teamFromJson(saved).language, QStringLiteral("auto"));
        saved.insert("language", "en");
        QCOMPARE(teamFromJson(saved).language, QStringLiteral("en"));
        saved.insert("language", "vi");
        QCOMPARE(teamFromJson(saved).language, QStringLiteral("vi"));
    }

    // Fixed system text is English; a user-authored Vietnamese note is untouched.
    void fixedBudgetDecisionAndDigestStayEnglishWithVietnameseContent() {
        const Decision asked = useTheBudget("vi");
        QCOMPARE(asked.kind, QStringLiteral("budget"));
        QCOMPARE(asked.question, QStringLiteral("The team used its 2 automatic turns. Continue?"));
        QCOMPARE(asked.choices, (QStringList{"Continue (2 more turns)", "Stop"}));
        QCOMPARE(asked.recommended, QStringLiteral("Continue (2 more turns)"));
        QVERIFY(asked.context.startsWith("Automatic turns are paused"));
        QCOMPARE(updates("paused").first().text,
                 QStringLiteral("Paused: the team used its 2 automatic turns."));
        QVERIFY(teams_->moveTask(team_, "T-1", "blocked", "dev1", "cần API key").ok());
        QVERIFY2(updates("state").first().text.endsWith(QStringLiteral(" → blocked: cần API key")),
                 qPrintable(updates("state").first().text));

        QVERIFY(teams_->answerDecision(team_, asked.id, asked.choices.first(), "user").ok());
        QVERIFY(!team().paused);
        QCOMPARE(updates("decision_answered").first().text,
                 QStringLiteral("%1 answered: Continue (2 more turns)").arg(asked.id));
        const auto digests = host_.of("decision");
        QCOMPARE(digests.size(), 1);
        QCOMPARE(digests.first().body,
                 QStringLiteral("Team Checkout: the user answered %1.\n"
                                "Question: The team used its 2 automatic turns. Continue?\n"
                                "Answer: Continue (2 more turns)")
                     .arg(asked.id));
    }

    // "Stop" keeps the team paused, as "Stop" does.
    void theSecondBudgetChoiceKeepsTheTeamPaused() {
        const Decision asked = useTheBudget("vi");
        QVERIFY(teams_->answerDecision(team_, asked.id, asked.choices.last(), "user").ok());
        QVERIFY(team().paused);
        QCOMPARE(team().pausedReason, QStringLiteral("user"));
    }

    // A decision raised in English still answers after the team changes to Vietnamese.
    void aDecisionRaisedBeforeTheChangeStillAnswers() {
        const Decision asked = useTheBudget("en");
        QCOMPARE(asked.choices.first(), QStringLiteral("Continue (2 more turns)"));
        QVERIFY(teams_->update(team_, {{"language", "vi"}}, "user").ok());
        QVERIFY(teams_->answerDecision(team_, asked.id, asked.choices.first(), "user").ok());
        QVERIFY(!team().paused);
    }

    void reviewBlocksRemainAcceptableInEitherLanguage_data() {
        QTest::addColumn<QString>("language");
        QTest::addColumn<bool>("noVerdict");
        for (const QString& language : {QStringLiteral("en"), QStringLiteral("vi")}) {
            QTest::newRow(qPrintable(language + "-limit")) << language << false;
            QTest::newRow(qPrintable(language + "-missing")) << language << true;
        }
    }

    void reviewBlocksRemainAcceptableInEitherLanguage() {
        QFETCH(QString, language);
        QFETCH(bool, noVerdict);
        start(language);
        QVERIFY(teams_->update(team_, {{"max_rounds", 1}}, "user").ok());
        const QString id = task("Cart");
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, "in_review", "dev1").ok());
        if (noVerdict) {
            for (int reply = 0; reply < 2; ++reply)
                finishReview(*teams_, "rev", "Still checking", false);
        } else {
            finishReview(*teams_, "rev", "Verdict: findings", false);
        }
        QCOMPARE(team().task(id)->state, state::blocked);
        QCOMPARE(team().task(id)->reviewBlock,
                 noVerdict ? QStringLiteral("no_verdict") : QStringLiteral("round_limit"));
        QVERIFY(team().task(id)->question.startsWith(noVerdict ? "Round " : "Review round"));
        // Persist the protocol reason across restart and a display-language change.
        teams_.reset();
        open();
        QVERIFY(teams_->update(team_, {{"language", language == "vi" ? "en" : "vi"}}, "user").ok());
        QVERIFY(!teams_->acceptReview(team_, id, "Local fixture review", "checked", "lead").ok());
        QCOMPARE(team().task(id)->state, state::blocked);
        QCOMPARE(team().task(id)->reviewBlock,
                 noVerdict ? QStringLiteral("no_verdict") : QStringLiteral("round_limit"));
    }

    void anInterruptedReviewKeepsItsProtocolReasonAndEnglishSystemMessage() {
        start("vi");
        const QString id = task("Cart");
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inReview, "dev1").ok());
        teams_->reviewTampered(
            "rev", firstReviews(team()).message,
            {QJsonObject{{"field", "worktree"}, {"before", "readable"}, {"after", "unreadable"}}});
        const auto* interrupted = team().task(id);
        QCOMPARE(interrupted->state, state::blocked);
        QCOMPARE(interrupted->round, 0);
        QCOMPARE(interrupted->reviewBlock, "review_interrupted");
        QVERIFY(interrupted->question.startsWith(QStringLiteral("Restore and verify")));
        QVERIFY(updates("review_interrupted")
                    .first()
                    .text.contains(QStringLiteral("No reviewer edit is inferred")));
    }

    void ordinaryBlocksCannotMasqueradeAsReviewBlocks() {
        start("vi");
        const QString id = task("Cart");
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QVERIFY(teams_
                    ->moveTask(team_, id, state::blocked, "dev1",
                               "Review round limit reached (1 of 1) with findings.")
                    .ok());
        const auto answer =
            teams_->acceptReview(team_, id, "Local fixture review", "Checked", "lead");
        QVERIFY(!answer.ok());
        QCOMPARE(answer.error->code, QStringLiteral("invalid_transition"));
    }

    // English is the source text: an English team keeps every word as before T-56.
    void anEnglishTeamReadsAsBefore() {
        const Decision asked = useTheBudget("en");
        QCOMPARE(asked.question, QStringLiteral("The team used its 2 automatic turns. Continue?"));
        QCOMPARE(asked.choices, (QStringList{"Continue (2 more turns)", "Stop"}));
        QCOMPARE(updates("team_created").first().text,
                 QStringLiteral("Team Checkout created with 4 members."));
        QVERIFY(teams_->moveTask(team_, "T-1", "blocked", "dev1", "needs a key").ok());
        QVERIFY2(updates("state").first().text.endsWith(QStringLiteral(" → blocked: needs a key")),
                 qPrintable(updates("state").first().text));
    }

    // Offline prompt fixtures: both prompt directions outrank an opposing legacy hint.
    void contentInstructionsPrioritizePromptOverLegacyPreferences() {
        using agent_messaging::teamInstructions;
        for (const QString& language :
             {QStringLiteral("auto"), QStringLiteral("vi"), QStringLiteral("en")}) {
            for (const QString& role : {QStringLiteral("leader"), QStringLiteral("developer"),
                                        QStringLiteral("reviewer")}) {
                const QString instructions = teamInstructions(role);
                const QString context = agent_messaging::conversationContext(
                    "4821", {.team = "Shop", .role = role, .language = language});
                QVERIFY(instructions.contains("The application interface is English"));
                QVERIFY(instructions.contains("A Vietnamese prompt calls for Vietnamese content"));
                QVERIFY(instructions.contains("an English prompt calls for English"));
                QVERIFY(instructions.contains("explicit content-language instructions"));
                QVERIFY(instructions.contains("retain the established content language"));
                QVERIFY(context.contains("never overrides them") == (language != "auto"));
                QVERIFY(instructions.contains("do not translate it retroactively"));
                QVERIFY(!instructions.contains("The team works in Vietnamese"));
            }
        }
    }
};

QTEST_GUILESS_MAIN(TeamLanguageTest)
#include "TeamLanguageTest.moc"
