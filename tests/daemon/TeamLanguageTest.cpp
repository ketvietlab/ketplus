// T-56: a team's language. The team stores the language its leader works in, logs a change in
// Updates, and the daemon writes its Updates, its own decisions and its messages to the leader
// in that language; an English team reads exactly as before. A fake runtime; no provider runs.
#include "TeamLanguage.h"
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "agents/AgentInstructions.h"
#include "storage/LocalJournal.h"
#include "teams/TeamRules.h"

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
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

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
        QCOMPARE(language::supported(), (QStringList{"en", "vi"}));
        QCOMPARE(language::fromLocale("vi-VN"), QStringLiteral("vi"));
        QCOMPARE(language::fromLocale("vi_VN"), QStringLiteral("vi"));
        QCOMPARE(language::fromLocale("fr-FR"), QStringLiteral("en"));
        QCOMPARE(language::orEnglish({}), QStringLiteral("en"));
        QCOMPARE(language::detect("Thanh toán bằng Adyen"), QStringLiteral("vi"));
        QCOMPARE(language::detect("Pay with Adyen"), QString{});
        QVERIFY(!teams::rules::validLanguage("vi"));
        const auto refused = teams::rules::validLanguage("fr");
        QVERIFY(refused);
        QCOMPARE(refused->code, QStringLiteral("invalid_payload"));
    }

    // Stored with the team, read back after a restart, changed in settings and logged.
    void theLanguageIsStoredAndAChangeIsLogged() {
        start("vi");
        QCOMPARE(team().language, QStringLiteral("vi"));
        QCOMPARE(updates("team_created").first().text,
                 QStringLiteral("Đã tạo team Checkout với 4 thành viên."));
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
        QCOMPARE(changed.first().text, QStringLiteral("The team now works in English."));
        QCOMPARE(changed.first().data.value("from").toString(), QStringLiteral("vi"));
        QCOMPARE(changed.first().data.value("to").toString(), QStringLiteral("en"));
        // The same language again changes nothing and logs nothing.
        QVERIFY(teams_->update(team_, {{"language", "en"}}, "user").ok());
        QCOMPARE(updates("language").size(), 1);
        QVERIFY(teams_->update(team_, {{"language", "vi"}}, "user").ok());
        QCOMPARE(updates("language").first().text,
                 QStringLiteral("Team giờ làm việc bằng Tiếng Việt."));
    }

    // A team saved before T-56 has no language: it is English.
    void aTeamWithoutALanguageIsEnglish() {
        QJsonObject saved = toJson(standardTeam());
        saved.remove("language");
        QCOMPARE(teamFromJson(saved).language, QStringLiteral("en"));
    }

    // The budget decision, its Update and the leader's digest in Vietnamese; the Vietnamese
    // first choice resumes the team as "Continue" does.
    void theBudgetDecisionAndTheDigestSpeakVietnamese() {
        const Decision asked = useTheBudget("vi");
        QCOMPARE(asked.kind, QStringLiteral("budget"));
        QCOMPARE(asked.question, QStringLiteral("Team đã dùng hết 2 lượt tự động. Tiếp tục?"));
        QCOMPARE(asked.choices, (QStringList{"Tiếp tục (thêm 2 lượt)", "Dừng"}));
        QCOMPARE(asked.recommended, QStringLiteral("Tiếp tục (thêm 2 lượt)"));
        QVERIFY(asked.context.startsWith("Lượt tự động tạm dừng"));
        QCOMPARE(updates("paused").first().text,
                 QStringLiteral("Tạm dừng: team đã dùng hết 2 lượt tự động."));
        QVERIFY(teams_->moveTask(team_, "T-1", "blocked", "dev1", "cần API key").ok());
        QVERIFY2(updates("state").first().text.endsWith(QStringLiteral(" → bị chặn: cần API key")),
                 qPrintable(updates("state").first().text));

        QVERIFY(teams_->answerDecision(team_, asked.id, asked.choices.first(), "user").ok());
        QVERIFY(!team().paused);
        QCOMPARE(updates("decision_answered").first().text,
                 QStringLiteral("%1 đã được trả lời: Tiếp tục (thêm 2 lượt)").arg(asked.id));
        const auto digests = host_.of("decision");
        QCOMPARE(digests.size(), 1);
        QCOMPARE(digests.first().body,
                 QStringLiteral("Team Checkout: người dùng đã trả lời %1.\n"
                                "Câu hỏi: Team đã dùng hết 2 lượt tự động. Tiếp tục?\n"
                                "Trả lời: Tiếp tục (thêm 2 lượt)")
                     .arg(asked.id));
    }

    // "Dừng" keeps the team paused, as "Stop" does.
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

    // The leader is told to write what people read in the team's language; code stays English.
    void theLeaderWritesInTheTeamLanguage() {
        using agent_messaging::teamInstructions;
        const QString leader =
            teamInstructions({.team = "Shop", .role = "leader", .language = "vi"});
        QVERIFY(leader.contains("The team works in Vietnamese"));
        QVERIFY(leader.contains("task titles"));
        QVERIFY(leader.contains("decisions"));
        QVERIFY(leader.contains("Code, commit messages and public pull request bodies stay"));
        const QString developer =
            teamInstructions({.team = "Shop", .role = "developer", .language = "vi"});
        QVERIFY(developer.contains("write your task notes and questions in Vietnamese"));
        QVERIFY(!teamInstructions({.team = "Shop", .role = "leader", .language = "en"})
                     .contains("Vietnamese"));
    }
};

QTEST_GUILESS_MAIN(TeamLanguageTest)
#include "TeamLanguageTest.moc"
