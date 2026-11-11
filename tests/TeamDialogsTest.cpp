// #465: New team and Add a developer. The defaults, the validation, the request that is sent,
// the refused create, the Claude-only reviewer and the small window, against a fake daemon.
// Run with QT_QPA_PLATFORM=offscreen.
#include "teams/TeamDialogs.h"
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetProfile.h"
#include "KetSegmentedControl.h"
#include "KetSelect.h"
#include "KetStyle.h"
#include "KetTextField.h"
#include "KetToggle.h"
#include "teams/TeamAgentDialog.h"
#include "teams/TeamDirectory.h"

#include <QApplication>
#include <QJsonArray>
#include <QLabel>
#include <QScrollArea>
#include <QStandardPaths>
#include <QtTest>

using namespace ketplus;

namespace {

QVariantMap model(const QString& provider, const QString& id, const QString& name,
                  bool isDefault = false) {
    return {{"provider", provider},
            {"model", id},
            {"displayName", name},
            {"isDefault", isDefault},
            {"defaultReasoningEffort", "medium"},
            {"supportedReasoningEfforts", QVariantList{QVariantMap{{"reasoningEffort", "low"}},
                                                       QVariantMap{{"reasoningEffort", "medium"}},
                                                       QVariantMap{{"reasoningEffort", "high"}}}}};
}

const QVariantList bothProviders{model("codex", "gpt-5", "GPT-5", true),
                                 model("claude", "opus", "Opus", true),
                                 model("claude", "sonnet", "Sonnet")};

struct FakeDaemon {
    QList<QPair<QString, QJsonObject>> sent;
    QHash<QString, QJsonObject> answers;
    TeamDirectory::Transport transport() {
        return [this](const QString& type, const QJsonObject& payload, TeamDirectory::Reply reply) {
            if (type != "team.list")
                sent.append({type, payload});
            const QJsonObject answer = answers.value(type, {{"ok", true}});
            reply(answer, answer.value("ok").toBool() ? QString{}
                                                      : answer.value("error_message").toString());
        };
    }
};

QJsonObject team(int developers) {
    QJsonArray members{QJsonObject{{"session_id", "lead"}, {"role", "leader"}}};
    for (int index = 0; index < developers; ++index)
        members.append(
            QJsonObject{{"session_id", QStringLiteral("dev%1").arg(index)}, {"role", "developer"}});
    members.append(QJsonObject{{"session_id", "rev"}, {"role", "reviewer"}});
    return {{"id", "7"}, {"name", "Shop"}, {"repository", "/repo"}, {"members", members}};
}

} // namespace

class TeamDialogsTest final : public QObject {
    Q_OBJECT

  private:
    FakeDaemon daemon_;
    std::unique_ptr<TeamDirectory> teams_;

  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ket::Style::install(*qApp);
        ket::Profile::setCurrent(ket::Profile::ketPlus());
    }

    void cleanup() { ket::Appearance::instance().setZoom(1.0); }

    void init() {
        daemon_ = {};
        teams_ = std::make_unique<TeamDirectory>();
        teams_->setTransport(daemon_.transport());
        teams_->setAvailable(true);
    }

    // Claude leads and reviews, the other ready provider develops, two developers.
    void theDefaultsMakeATeamOfFour() {
        NewTeamForm form(teams_.get(), "/repo", bothProviders);
        QCOMPARE(form.agent("leader").provider, QStringLiteral("claude"));
        QCOMPARE(form.agent("leader").model, QStringLiteral("opus"));
        QCOMPARE(form.agent("leader").effort, QStringLiteral("medium"));
        QCOMPARE(form.agent("developer").provider, QStringLiteral("codex"));
        QCOMPARE(form.agent("reviewer").provider, QStringLiteral("claude"));
        QCOMPARE(form.memberCount(), 4);
        QCOMPARE(form.agentButton("leader")->text(), QStringLiteral("Claude · Opus · medium"));
        QVERIFY(form.hint().isEmpty());
        QVERIFY(form.findChild<ket::Radio*>("teamLeaderMerges")->isChecked());
        QVERIFY(form.findChild<ket::Checkbox*>("teamLeaderWritesRequirements")->isChecked());
        QSignalSpy counted(&form, &NewTeamForm::memberCountChanged);
        form.developerCount()->setCurrentIndex(4);
        QCOMPARE(counted.last().first().toInt(), 7);
    }

    void oneProviderRunsEverythingAndSaysSo() {
        NewTeamForm claude(teams_.get(), "/repo", {model("claude", "opus", "Opus")});
        QCOMPARE(claude.agent("developer").provider, QStringLiteral("claude"));
        QVERIFY(claude.hint().contains("second provider"));
        // Without Claude there is no reviewer, and the dialog says where to set it up.
        NewTeamForm codex(teams_.get(), "/repo", {model("codex", "gpt-5", "GPT-5")});
        codex.nameField()->setText("Shop");
        QVERIFY(codex.agent("reviewer").isEmpty());
        QVERIFY(!codex.validate());
        QVERIFY(codex.error("reviewer").contains("Claude"));
        QVERIFY(daemon_.sent.isEmpty());
    }

    void anEmptyNameIsMarkedAndNothingIsSent() {
        NewTeamForm form(teams_.get(), "/repo", bothProviders);
        form.nameField()->setText("   ");
        form.submit({});
        QVERIFY(!form.error("name").isEmpty());
        QVERIFY(form.nameField()->isInvalid());
        QVERIFY(daemon_.sent.isEmpty());
        form.nameField()->setText("Shop");
        QVERIFY(form.error("name").isEmpty());
    }

    void createSendsOneRequestWithThePlan() {
        NewTeamForm form(teams_.get(), "/repo", bothProviders);
        form.nameField()->setText(" Shop ");
        form.findChild<ket::TextField*>("teamGoalField")->setText("Checkout in one page");
        form.findChild<ket::TextField*>("teamIntegrationBranchField")->setText("team/shop");
        form.findChild<ket::Radio*>("teamUserMerges")->setChecked(true);
        // Workspace unless chosen; the help says what the choice means.
        auto* permissions = form.findChild<ket::Select*>("teamPermissions");
        QCOMPARE(permissions->currentData().toString(), QStringLiteral("ask"));
        QCOMPARE(form.payload().value("execution_policy").toString(), QStringLiteral("ask"));
        QVERIFY(!form.payload().value("risk_acknowledged").toBool());
        permissions->setCurrentIndex(2);
        QCOMPARE(permissions->currentText(), QStringLiteral("Full access"));
        form.developerCount()->setCurrentIndex(2);
        form.setAgent("developer", {"codex", "gpt-5", "high"});
        daemon_.answers.insert(
            "team.create", {{"ok", true}, {"team", QJsonObject{{"id", "9"}, {"name", "Shop"}}}});
        QJsonObject created;
        form.submit([&](const QJsonObject& team) { created = team; });
        QCOMPARE(daemon_.sent.size(), 1);
        const QJsonObject sent = daemon_.sent.first().second;
        QCOMPARE(daemon_.sent.first().first, QStringLiteral("team.create"));
        QCOMPARE(sent.value("repository").toString(), QStringLiteral("/repo"));
        QCOMPARE(sent.value("name").toString(), QStringLiteral("Shop"));
        QCOMPARE(sent.value("goal").toString(), QStringLiteral("Checkout in one page"));
        QCOMPARE(sent.value("integration_branch").toString(), QStringLiteral("team/shop"));
        QCOMPARE(sent.value("merge_policy").toString(), QStringLiteral("user"));
        QCOMPARE(sent.value("execution_policy").toString(), QStringLiteral("full"));
        QVERIFY(sent.value("risk_acknowledged").toBool());
        QVERIFY(sent.value("leader_writes_requirements").toBool());
        const QJsonObject plan = sent.value("plan").toObject();
        QCOMPARE(plan.value("developers").toObject().value("count").toInt(), 3);
        QCOMPARE(plan.value("developers").toObject().value("effort").toString(),
                 QStringLiteral("high"));
        QCOMPARE(plan.value("leader").toObject().value("provider").toString(),
                 QStringLiteral("claude"));
        QCOMPARE(plan.value("reviewer").toObject().value("model").toString(),
                 QStringLiteral("opus"));
        QCOMPARE(created.value("id").toString(), QStringLiteral("9"));
        QVERIFY(teams_->team("9"));
    }

    // A refusal keeps what was typed and shows the error at the role it names.
    void aRefusedCreateStaysOpenWithTheError() {
        ket::Dialog* dialog = nullptr;
        QWidget window;
        window.resize(1024, 640);
        window.show();
        dialog = NewTeamForm::present(&window, teams_.get(), "/repo", bothProviders);
        auto* form = dialog->findChild<NewTeamForm*>();
        form->nameField()->setText("Shop");
        daemon_.answers.insert("team.create", {{"ok", false},
                                               {"error_code", "provider_unavailable"},
                                               {"error_message", "Sign in to Codex first."},
                                               {"role", "developer"}});
        dialog->findChild<ket::Button*>("teamCreateButton")->click();
        QCOMPARE(daemon_.sent.size(), 1);
        QVERIFY(dialog->isOpen());
        QCOMPARE(form->error("developer"), QStringLiteral("Sign in to Codex first."));
        QCOMPARE(form->nameField()->text(), QStringLiteral("Shop"));
        QVERIFY(!teams_->teams().size());
        // A name refusal lands on the name.
        daemon_.answers.insert(
            "team.create", {{"ok", false},
                            {"error_code", "invalid_payload"},
                            {"error_message", "The name of The team is longer than 120 bytes."}});
        dialog->findChild<ket::Button*>("teamCreateButton")->click();
        QVERIFY(!form->error("name").isEmpty());
        QVERIFY(form->error("developer").isEmpty());
        dialog->reject();
    }

    void theReviewerChoosesClaudeOnly() {
        TeamAgentChooser chooser(bothProviders, {"claude", "opus", "medium"}, "claude");
        QCOMPARE(chooser.providers()->count(), 1);
        QCOMPARE(chooser.providers()->currentData().toString(), QStringLiteral("claude"));
        QVERIFY(!chooser.providers()->isEnabled());
        QCOMPARE(chooser.modelSelect()->count(), 2);
        chooser.modelSelect()->setCurrentIndex(1);
        QCOMPARE(chooser.agent().model, QStringLiteral("sonnet"));
        QCOMPARE(chooser.agent().effort, QStringLiteral("medium"));
        // Another role offers every ready provider.
        TeamAgentChooser open(bothProviders, {"claude", "opus", {}});
        QCOMPARE(open.providers()->count(), 2);
        open.providers()->setCurrentIndex(open.providers()->findData("codex"));
        QCOMPARE(open.agent().model, QStringLiteral("gpt-5"));
    }

    // At 1024 x 640 the card stays inside the window and its buttons stay reachable.
    void theDialogFitsASmallWindow() {
        QWidget window;
        window.resize(1024, 640);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* dialog = NewTeamForm::present(&window, teams_.get(), "/repo", bothProviders);
        QApplication::processEvents();
        const QRect card = dialog->card()->geometry();
        QVERIFY2(card.bottom() <= window.height() && card.top() >= 0,
                 qPrintable(QStringLiteral("card %1..%2").arg(card.top()).arg(card.bottom())));
        auto* create = dialog->findChild<ket::Button*>("teamCreateButton");
        QVERIFY(create->isVisible());
        QCOMPARE(create->text(), QStringLiteral("Create with 4 members"));
        QVERIFY(dialog->findChild<QWidget*>("teamDialogScroll"));
        dialog->reject();
    }

    // In a narrow window the card is narrower than its 560 px: the form follows it instead of
    // keeping its width behind the card's edge, where the labels were cut off.
    void theFormFollowsANarrowCard() {
        QWidget window;
        window.resize(480, 640);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* dialog = NewTeamForm::present(&window, teams_.get(), "/repo", bothProviders);
        QApplication::processEvents();
        auto* scroll = dialog->findChild<QScrollArea*>("teamDialogScroll");
        auto* form = dialog->findChild<NewTeamForm*>();
        QStringList widest;
        for (const QWidget* child : form->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
            widest.append(
                QStringLiteral("%1/%2=%3")
                    .arg(QString::fromLatin1(child->metaObject()->className()), child->objectName())
                    .arg(child->minimumSizeHint().width()));
        QTRY_VERIFY2(form->width() <= scroll->viewport()->width(),
                     qPrintable(QStringLiteral("form %1 > viewport %2: %3")
                                    .arg(form->width())
                                    .arg(scroll->viewport()->width())
                                    .arg(widest.join(QLatin1Char(' ')))));
        QVERIFY(dialog->card()->geometry().right() <= window.width());
        // The count sits above the developers' agent, and both end inside the form.
        const QWidget* count = form->developerCount();
        const QWidget* agent = form->agentButton("developer");
        QVERIFY(count->geometry().bottom() < agent->geometry().top());
        QVERIFY(count->mapTo(form, count->rect().bottomRight()).x() < form->width());
        QVERIFY(agent->mapTo(form, agent->rect().bottomRight()).x() < form->width());
        if (qEnvironmentVariableIsSet("KETPLUS_TEST_GRAB"))
            window.grab().save(qEnvironmentVariable("KETPLUS_TEST_GRAB"));
        dialog->reject();

        // A window with room keeps them side by side.
        QWidget wide;
        wide.resize(1024, 640);
        wide.show();
        QVERIFY(QTest::qWaitForWindowExposed(&wide));
        auto* roomy = NewTeamForm::present(&wide, teams_.get(), "/repo", bothProviders);
        QApplication::processEvents();
        auto* roomyForm = roomy->findChild<NewTeamForm*>();
        QTRY_VERIFY(roomyForm->developerCount()->geometry().right() <
                    roomyForm->agentButton("developer")->geometry().left());
        roomy->reject();
    }

    // The same at 125 % and 150 %, where the card is narrower still and the controls wider:
    // opened narrow, zoomed while open, and while the window is resized. Every control ends
    // inside the form, and the form inside the card.
    void theFormFitsANarrowCardAtEveryZoom() {
        const QVariantList longNames{
            model("codex", "gpt-6.1-super", "GPT-6.1-Super-Long-Context (recommended)", true),
            model("claude", "default", "Default (recommended) with a very long name", true)};
        const auto misfit = [](ket::Dialog* dialog, const QString& when) -> QString {
            auto* scroll = dialog->findChild<QScrollArea*>("teamDialogScroll");
            auto* form = dialog->findChild<NewTeamForm*>();
            QString widest;
            for (const QWidget* child : form->findChildren<QWidget*>())
                if (child->minimumSizeHint().width() >= scroll->viewport()->width() ||
                    child->minimumWidth() >= scroll->viewport()->width())
                    widest += QStringLiteral(" %1/%2=%3")
                                  .arg(QString::fromLatin1(child->metaObject()->className()),
                                       child->objectName())
                                  .arg(child->minimumSizeHint().width());
            if (form->width() > scroll->viewport()->width())
                return (QStringLiteral("%1: form %2 > viewport %3 (needs %4, scroll %5, card %6)") +
                        widest)
                    .arg(when)
                    .arg(form->width())
                    .arg(scroll->viewport()->width())
                    .arg(form->minimumSizeHint().width())
                    .arg(scroll->width())
                    .arg(dialog->card()->width());
            for (const ket::FieldRow* row : form->findChildren<ket::FieldRow*>()) {
                const QWidget* control = row->control();
                if (control->mapTo(form, control->rect().bottomRight()).x() >= form->width())
                    return QStringLiteral("%1: a control ends outside the form").arg(when);
            }
            const QRect card = dialog->card()->geometry();
            if (card.left() < 0 || card.right() >= dialog->parentWidget()->width())
                return QStringLiteral("%1: the card leaves the window").arg(when);
            return {};
        };
        const auto settled = [&](ket::Dialog* dialog, const QString& when) {
            QString problem;
            // The card is placed again after a resize or a zoom; give it a moment.
            for (int attempt = 0; attempt < 20; ++attempt) {
                QTest::qWait(25);
                problem = misfit(dialog, when);
                if (problem.isEmpty())
                    break;
            }
            return problem;
        };
        QStringList problems;
        for (const qreal zoom : {1.0, 1.25, 1.5}) {
            ket::Appearance::instance().setZoom(zoom);
            QWidget window;
            window.resize(480, 640);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto* dialog = NewTeamForm::present(&window, teams_.get(), "/repo", longNames);
            problems << settled(dialog, QStringLiteral("opened at %1").arg(zoom));
            for (const int width : {1024, 480, 640, 480}) {
                window.resize(width, 640);
                problems << settled(dialog, QStringLiteral("%1 px at %2").arg(width).arg(zoom));
            }
            dialog->reject();
        }
        ket::Appearance::instance().setZoom(1.0);
        QWidget window;
        window.resize(480, 640);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* dialog = NewTeamForm::present(&window, teams_.get(), "/repo", bothProviders);
        for (const qreal zoom : {1.25, 1.5, 1.25, 1.0}) {
            ket::Appearance::instance().setZoom(zoom);
            problems << settled(dialog, QStringLiteral("zoomed to %1 while open").arg(zoom));
        }
        // With room again the labels go back beside their controls.
        window.resize(1024, 640);
        auto* form = dialog->findChild<NewTeamForm*>();
        QTRY_VERIFY(form->developerCount()->geometry().right() <
                    form->agentButton("developer")->geometry().left());
        for (const ket::FieldRow* row : form->findChildren<ket::FieldRow*>())
            QVERIFY(!row->isStacked());
        dialog->reject();
        problems.removeAll(QString{});
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QLatin1Char('\n'))));
    }

    // A long model name elides inside its button instead of widening the form past the card.
    void longModelNamesStayInsideTheCard() {
        const QVariantList longNames{
            model("codex", "gpt-6.1-super", "GPT-6.1-Super-Long-Context (recommended)", true),
            model("claude", "default", "Default (recommended) with a very long name", true)};
        QWidget window;
        window.resize(1024, 640);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* dialog = NewTeamForm::present(&window, teams_.get(), "/repo", longNames);
        QApplication::processEvents();
        auto* scroll = dialog->findChild<QScrollArea*>("teamDialogScroll");
        QVERIFY(scroll);
        auto* form = dialog->findChild<NewTeamForm*>();
        QTRY_VERIFY2(form->width() <= scroll->viewport()->width(),
                     qPrintable(QStringLiteral("form %1 > viewport %2")
                                    .arg(form->width())
                                    .arg(scroll->viewport()->width())));
        for (const QString& role : {"leader", "developer", "reviewer"}) {
            ket::Button* button = form->agentButton(role);
            QVERIFY(button->geometry().right() < form->width());
            QVERIFY(button->toolTip().contains(QStringLiteral("recommended")));
            QVERIFY(button->sizeHint().width() <= button->width() + 1);
        }
        dialog->reject();
    }

    void addingADeveloperNeedsANameAndStopsAtFive() {
        AddDeveloperForm form(teams_.get(), team(2), bothProviders);
        QVERIFY(form.hint().contains("single reviewer"));
        form.submit({});
        QVERIFY(daemon_.sent.isEmpty());
        QVERIFY(form.nameField()->isInvalid());
        form.nameField()->setText("Payments");
        form.laneField()->setText("API");
        form.pathsField()->setText("src/api, docs/api");
        bool added = false;
        form.submit([&] { added = true; });
        QVERIFY(added);
        const QJsonObject sent = daemon_.sent.first().second;
        QCOMPARE(daemon_.sent.first().first, QStringLiteral("team.member.add"));
        QCOMPARE(sent.value("team_id").toString(), QStringLiteral("7"));
        QCOMPARE(sent.value("name").toString(), QStringLiteral("Payments"));
        QCOMPARE(sent.value("lane").toString(), QStringLiteral("API"));
        QCOMPARE(sent.value("paths").toArray(), (QJsonArray{"src/api", "docs/api"}));
        QCOMPARE(sent.value("provider").toString(), QStringLiteral("codex"));
        // One developer: no queue hint yet.
        AddDeveloperForm small(teams_.get(), team(1), bothProviders);
        QVERIFY(small.hint().isEmpty());
        // Five: the entry is disabled.
        QWidget window;
        window.resize(1024, 640);
        window.show();
        auto* dialog = AddDeveloperForm::present(&window, teams_.get(), team(5), bothProviders);
        QVERIFY(!dialog->findChild<ket::Button*>("addDeveloperButton")->isEnabled());
        QVERIFY(dialog->findChild<AddDeveloperForm*>()->hint().contains("five"));
        dialog->reject();
    }
};

QTEST_MAIN(TeamDialogsTest)
#include "TeamDialogsTest.moc"
