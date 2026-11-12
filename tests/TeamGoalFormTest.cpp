// ADR 0114: the goal of New team is a multi-line kit field with `@agent/` mentions of the
// conversations on this computer, and the team page folds a long goal. Against a fake daemon.
// Run with QT_QPA_PLATFORM=offscreen.
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetExpandableText.h"
#include "KetFieldRow.h"
#include "KetMentionPicker.h"
#include "KetMentionTokens.h"
#include "KetProfile.h"
#include "KetStyle.h"
#include "KetTextArea.h"
#include "KetTextField.h"
#include "teams/TeamDialogs.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamGoal.h"

#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QMainWindow>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextEdit>
#include <QtTest>

using namespace ketplus;

namespace {

QVariantMap model(const QString& provider, const QString& id) {
    return {{"provider", provider},
            {"model", id},
            {"displayName", id},
            {"isDefault", true},
            {"defaultReasoningEffort", "medium"},
            {"supportedReasoningEfforts",
             QVariantList{QVariantMap{{"reasoningEffort", "medium"}}}}};
}

const QVariantList models{model("codex", "gpt-5"), model("claude", "opus")};

QVariantList directory() {
    return {QVariantMap{{"number", "8213"},
                        {"title", "Finish the repository picker"},
                        {"provider", "claude"},
                        {"path", "/work/shop"}},
            QVariantMap{{"number", "8215"}, {"title", "Follow up on the new UI"}, {"provider", "codex"}}};
}

void type(QWidget* widget, const QString& text) {
    for (const QChar character : text)
        QTest::keyClick(widget, character.toLatin1());
}

} // namespace

class TeamGoalFormTest final : public QObject {
    Q_OBJECT

  private:
    QList<QPair<QString, QJsonObject>> sent_;
    std::unique_ptr<TeamDirectory> teams_;

    std::unique_ptr<NewTeamForm> form() {
        auto made = std::make_unique<NewTeamForm>(teams_.get(), "/repo", models, directory);
        made->nameField()->setText("Shop");
        made->resize(560, 900);
        made->show();
        made->activateWindow();
        [[maybe_unused]] const bool active = QTest::qWaitForWindowActive(made.get());
        return made;
    }

  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ket::Style::install(*qApp);
        ket::Profile::setCurrent(ket::Profile::ketPlus());
    }

    void init() {
        sent_.clear();
        teams_ = std::make_unique<TeamDirectory>();
        teams_->setTransport([this](const QString& type, const QJsonObject& payload,
                                    TeamDirectory::Reply reply) {
            if (type != "team.list" && type != "team.settings.get")
                sent_.append({type, payload});
            reply({{"ok", true}, {"team", QJsonObject{{"id", "9"}}}}, {});
        });
        teams_->setAvailable(true);
    }

    void theGoalShowsSixLinesAndGrowsThenScrolls() {
        const auto made = form();
        ket::TextArea* goal = made->goalField();
        QVERIFY(goal->visibleLines() >= 6);
        QVERIFY(!goal->placeholderText().contains("One sentence"));
        QVERIFY(goal->placeholderText().contains("@"));
        QCOMPARE(goal->maxLength(), team_goal::maximumCharacters);
        const int empty = goal->height();
        goal->setText(QStringList(9, QStringLiteral("A line of context")).join('\n'));
        QCOMPARE(goal->visibleLines(), 9);
        QVERIFY(goal->height() > empty);
        goal->setText(QStringList(40, QStringLiteral("A line of context")).join('\n'));
        QCOMPARE(goal->visibleLines(), team_goal::growLines);
        QVERIFY(goal->editor()->verticalScrollBar()->maximum() > 0);
    }

    void returnBreaksTheLineAndPasteKeepsParagraphs() {
        const auto made = form();
        QTextEdit* editor = made->goalField()->editor();
        editor->setFocus();
        type(editor, "Ship");
        QTest::keyClick(editor, Qt::Key_Return);
        type(editor, "it");
        QCOMPARE(made->goalField()->text(), QStringLiteral("Ship\nit"));
        QGuiApplication::clipboard()->setText("First paragraph.\n\nSecond paragraph.");
        editor->paste();
        QCOMPARE(made->goalField()->text(),
                 QStringLiteral("Ship\nitFirst paragraph.\n\nSecond paragraph."));
        QVERIFY(sent_.isEmpty());
    }

    void cmdReturnCreatesFromTheGoal() {
        QMainWindow window;
        window.resize(1024, 900);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        ket::Dialog* dialog =
            NewTeamForm::present(&window, teams_.get(), "/repo", models, {}, directory);
        auto* made = dialog->findChild<NewTeamForm*>();
        made->nameField()->setText("Shop");
        made->goalField()->editor()->setFocus();
        QTest::keyClick(window.windowHandle(), Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(sent_.size(), 1);
        QCOMPARE(sent_.first().first, QStringLiteral("team.create"));
    }

    void typingAtOffersConversationsAndAMentionIsOneToken() {
        const auto made = form();
        ket::TextArea* goal = made->goalField();
        QTextEdit* editor = goal->editor();
        editor->setFocus();
        type(editor, "Ask @");
        // The goal mentions only conversations: a plain @ completes to @agent/.
        QCOMPARE(goal->text(), QStringLiteral("Ask @agent/"));
        QTRY_VERIFY(goal->isMentionOpen());
        QCOMPARE(goal->mentionPicker()->items().size(), 2);
        type(editor, "82");
        QCOMPARE(goal->mentionPicker()->items().size(), 2);
        type(editor, "13");
        QCOMPARE(goal->mentionPicker()->items().size(), 1);
        QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(goal->text(), QStringLiteral("Ask @agent/8213 "));
        QCOMPARE(goal->mentions()->numbers(), QStringList{"8213"});
        QVERIFY(editor->accessibleDescription().contains("Finish the repository picker"));
        QTest::keyClick(editor, Qt::Key_Backspace); // the space
        QTest::keyClick(editor, Qt::Key_Backspace); // the whole mention
        QCOMPARE(goal->text(), QStringLiteral("Ask "));
        // An e-mail address is not a mention.
        type(editor, "me@");
        QCOMPARE(goal->text(), QStringLiteral("Ask me@"));
    }

    void anUnknownMentionIsShownAndStopsCreate() {
        const auto made = form();
        made->goalField()->setText("Ask @agent/8213 and @agent/9999.");
        // Shown under the goal as soon as it is typed, before Create.
        const auto shown = [&made](const QString& part) {
            for (const QLabel* label : made->findChildren<QLabel*>())
                if (label->isVisible() && label->text().contains(part))
                    return true;
            return false;
        };
        QTRY_VERIFY(shown("@agent/9999 is not a conversation"));
        QVERIFY(!shown("@agent/8213 is not"));
        QVERIFY(made->goalField()->isInvalid());
        made->submit({});
        QVERIFY(made->error("goal").contains("@agent/9999"));
        QVERIFY(sent_.isEmpty());
        made->goalField()->setText("Ask @agent/8213.");
        QVERIFY(!shown("is not a conversation"));
        QVERIFY(!made->goalField()->isInvalid());
        made->submit({});
        QCOMPARE(sent_.size(), 1);
    }

    void aLongGoalIsSentWholeAndAboveTheLimitNothingIsSent() {
        const auto made = form();
        QString goal = QStringLiteral("Ship the checkout.\n\n");
        while (goal.size() < 9000)
            goal += QStringLiteral("The cart keeps its items across devices.\n");
        made->goalField()->setText(goal);
        made->submit({});
        QCOMPARE(sent_.size(), 1);
        QCOMPARE(sent_.first().second.value("goal").toString(), goal.trimmed());

        const auto over = form();
        over->goalField()->setText(QString(team_goal::maximumCharacters + 5, QLatin1Char('x')));
        QVERIFY(over->goalField()->isOverLimit());
        over->submit({});
        QCOMPARE(sent_.size(), 1);
        QVERIFY(over->error("goal").contains(QLocale().toString(team_goal::maximumCharacters + 5)));
        // The text is never cut.
        QCOMPARE(over->goalField()->text().size(), team_goal::maximumCharacters + 5);
    }

    void theTeamPageFoldsTheGoalAndOpensAMention() {
        const QString goal = QStringLiteral("Ship the picker.\nAsk @agent/8213 for context.\n"
                                            "Line three.\nLine four.\nLine five.");
        QWidget page;
        page.resize(480, 400);
        ket::ExpandableText* text = team_goal::foldedGoal(goal, directory(), &page);
        text->setGeometry(0, 0, 480, 300);
        page.show();
        page.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&page));
        QCOMPARE(text->foldedLines(), team_goal::foldedLines);
        QVERIFY(text->isFoldable());
        QCOMPARE(text->visibleLineCount(), team_goal::foldedLines);
        text->setExpanded(true);
        QCOMPARE(text->visibleLineCount(), 5);
        QSignalSpy opened(text, &ket::ExpandableText::mentionActivated);
        text->setFocus();
        QTest::keyClick(text, Qt::Key_Return);
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.first().first().toString(), QStringLiteral("8213"));
        QVERIFY(text->accessibleDescription().contains("Finish the repository picker"));
    }
};

QTEST_MAIN(TeamGoalFormTest)
#include "TeamGoalFormTest.moc"
