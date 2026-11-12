// ADR 0114: a team's goal is long, kept whole, and its `@agent/<number>` mentions are listed
// for the leader without messaging anyone. The coordinator runs on a real journal with a fake
// runtime. No provider is started.
#include "TeamFakeHost.h"
#include "storage/LocalJournal.h"
#include "teams/TeamRemoteView.h"
#include "teams/TeamRules.h"

#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace ketplus::daemon;
using namespace team_test;

namespace {

// A goal of `characters` characters in paragraphs, with line breaks and a mention.
QString longGoal(const qsizetype characters) {
    QString goal =
        QStringLiteral("Ship the checkout.\n\nContext: ask @agent/8213 about the cart.\n");
    const QString paragraph = QStringLiteral("The cart keeps its items across devices; "
                                             "prices come from the catalog.\n\n");
    while (goal.size() < characters)
        goal += paragraph;
    goal.truncate(characters - 1);
    return goal + QLatin1Char('.');
}

// A goal of exactly `bytes` UTF-8 bytes in Vietnamese, symbols and CJK (about 1.6 bytes a
// character), with a mention: it reaches the byte limit before the character limit.
QString nonAsciiGoal(const qsizetype bytes) {
    const QString phrase = QStringLiteral("Thuế hoá đơn — giỏ hàng ✓ đồng bộ ✓ 購物 ");
    QString goal = QStringLiteral("Hỏi @agent/8213 trước.\n");
    while (goal.toUtf8().size() + phrase.toUtf8().size() <= bytes)
        goal += phrase;
    while (goal.toUtf8().size() < bytes)
        goal += QLatin1Char('.');
    return goal;
}

} // namespace

class TeamGoalTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;

    TeamResult create(const QString& goal) {
        Team draft = standardTeam();
        draft.goal = goal;
        return teams_->create(draft, "user");
    }

    QString leaderTurn() const {
        for (const TeamMessage& message : host_.messages)
            if (message.toSession == QStringLiteral("lead") && message.kind == "team")
                return message.body;
        return {};
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
        journal_ = std::make_unique<LocalJournal>(directory_.filePath("journal.sqlite3"));
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->load();
    }

    void aLongGoalReachesTheLeaderWhole() {
        const QString goal = longGoal(9000);
        QCOMPARE(goal.size(), 9000);
        const TeamResult result = create(goal);
        QVERIFY2(result.ok(), qPrintable(result.error.value_or(TeamError{}).message));
        QCOMPARE(teams_->team(result.teamId)->goal, goal);
        const QString turn = leaderTurn();
        QVERIFY(turn.contains(QStringLiteral("Goal: ") + goal + QLatin1Char('\n')));

        // Kept across a restart and sent to paired devices whole.
        teams_.reset();
        journal_ = std::make_unique<LocalJournal>(directory_.filePath("journal.sqlite3"));
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->load();
        const Team* team = teams_->team(result.teamId);
        QVERIFY(team);
        QCOMPARE(team->goal, goal);
        const remote::Ids ids{[](const QString&) { return QStringLiteral("repo"); },
                              [](const QString&) { return QStringLiteral("wt"); }};
        QCOMPARE(remote::summary(teams_->teamJson(*team), ids).value("goal").toString(), goal);
    }

    void aGoalAboveTheLimitIsRefusedNotCut() {
        const QString goal(rules::maximumGoalCharacters + 1, QLatin1Char('x'));
        const TeamResult refused = create(goal);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error->code, QStringLiteral("invalid_payload"));
        QVERIFY(refused.error->message.contains(QStringLiteral("16001")));
        QVERIFY(host_.messages.isEmpty());

        const TeamResult made = create(QStringLiteral("Ship"));
        QVERIFY(made.ok());
        const TeamResult changed = teams_->update(made.teamId, {{"goal", goal}}, "user");
        QVERIFY(!changed.ok());
        QCOMPARE(teams_->team(made.teamId)->goal, QStringLiteral("Ship"));
        const QString longest(rules::maximumGoalCharacters, QLatin1Char('y'));
        QVERIFY(teams_->update(made.teamId, {{"goal", longest}}, "user").ok());
        QCOMPARE(teams_->team(made.teamId)->goal, longest);
    }

    // A goal of accented letters, other scripts and symbols: its bytes, not its characters,
    // are what one message to the leader holds, so the limit is on both.
    void aNonAsciiGoalIsBoundedInBytesNotCut() {
        const QString longest = nonAsciiGoal(rules::maximumGoalBytes);
        QCOMPARE(longest.toUtf8().size(), rules::maximumGoalBytes);
        QVERIFY(longest.size() < rules::maximumGoalCharacters);
        const TeamResult made = create(longest);
        QVERIFY2(made.ok(), qPrintable(made.error.value_or(TeamError{}).message));
        QCOMPARE(teams_->team(made.teamId)->goal, longest);
        QVERIFY(leaderTurn().contains(QStringLiteral("Goal: ") + longest + QLatin1Char('\n')));

        const QString over = longest + QStringLiteral("ế");
        QVERIFY(over.size() < rules::maximumGoalCharacters);
        const TeamResult refused = create(over);
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error->code, QStringLiteral("invalid_payload"));
        QVERIFY(refused.error->message.contains(QString::number(over.toUtf8().size())));
        QVERIFY(!teams_->update(made.teamId, {{"goal", over}}, "user").ok());
        QCOMPARE(teams_->team(made.teamId)->goal, longest);
    }

    // Many mentions: the first ones are listed with short titles, the rest counted, so the
    // first turn still fits in one message.
    void aLongListOfMentionsIsBounded() {
        host_.add("ctx");
        host_.conversations["ctx"].number = "1";
        host_.conversations["ctx"].title = QString(200, QLatin1Char('t'));
        QStringList mentions;
        for (int number = 1; number <= rules::maximumListedMentions + 5; ++number)
            mentions << QStringLiteral("@agent/%1").arg(number);
        QVERIFY(create(QStringLiteral("Ask ") + mentions.join(QLatin1Char(' '))).ok());
        const QString turn = leaderTurn();
        QVERIFY(turn.contains(QStringLiteral("- #1 ") +
                              QString(rules::maximumMentionTitleCharacters - 1, QLatin1Char('t')) +
                              QChar(0x2026) + QLatin1Char('\n')));
        QVERIFY(turn.contains(QStringLiteral("- #%1 ").arg(rules::maximumListedMentions)));
        QVERIFY(!turn.contains(QStringLiteral("- #%1 ").arg(rules::maximumListedMentions + 1)));
        QVERIFY(turn.contains(QStringLiteral("- and 5 more; their numbers are in the goal.")));
    }

    void theLeaderIsToldWhomTheGoalMentions() {
        host_.add("ctx");
        host_.conversations["ctx"].number = "8213";
        host_.conversations["ctx"].title = "Finish the repository picker";
        const TeamResult result = create(QStringLiteral(
            "Ship the picker.\nAsk @agent/8213 first, (@agent/9999) too, and @agent/8213 again. "
            "Not a mention: me@agent/77."));
        QVERIFY(result.ok());
        const QString turn = leaderTurn();
        QVERIFY(turn.contains(QStringLiteral("Conversations the goal mentions:\n"
                                             "- #8213 Finish the repository picker\n"
                                             "- #9999 (no conversation with this number on "
                                             "this computer)\n")));
        QVERIFY(turn.contains(QStringLiteral("agents_send")));
        QVERIFY(!turn.contains(QStringLiteral("#77")));
        // Creating the team messaged only its leader.
        for (const TeamMessage& message : host_.messages)
            QCOMPARE(message.toSession, QStringLiteral("lead"));
    }

    void aGoalWithoutMentionsListsNone() {
        QVERIFY(create(QStringLiteral("Ship the checkout")).ok());
        QVERIFY(!leaderTurn().contains(QStringLiteral("mentions")));
        QCOMPARE(rules::goalMentions(QStringLiteral("@agent/12 and \"@agent/a-b_c\"")),
                 (QStringList{"12", "a-b_c"}));
    }
};

QTEST_GUILESS_MAIN(TeamGoalTest)
#include "TeamGoalTest.moc"
