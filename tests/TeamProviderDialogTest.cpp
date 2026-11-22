// T-3: Change provider… for a team. Every member by default, a member left out, a reviewer that
// stays without Full access or moves with it, the request sent, each member's result and a refusal,
// against a fake daemon. Run with QT_QPA_PLATFORM=offscreen.
#include "teams/TeamProviderDialog.h"
#include "KetAppearance.h"
#include "KetBanner.h"
#include "KetButton.h"
#include "KetChoice.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetSelect.h"
#include "KetToggle.h"
#include "teams/TeamAgentDialog.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPageParts.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QJsonArray>
#include <QStandardItemModel>
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

const QVariantList models{model("codex", "gpt-5", "GPT-5", true),
                          model("claude", "opus", "Opus", true),
                          model("claude", "sonnet", "Sonnet")};

struct FakeDaemon {
    QList<QPair<QString, QJsonObject>> sent;
    QHash<QString, QJsonObject> answers;
    TeamDirectory::Transport transport() {
        return [this](const QString& type, const QJsonObject& payload, TeamDirectory::Reply reply) {
            if (type != "team.list" && type != "team.settings.get")
                sent.append({type, payload});
            const QJsonObject answer = answers.value(type, {{"ok", true}});
            reply(answer, answer.value("ok").toBool() ? QString{}
                                                      : answer.value("error_message").toString());
        };
    }
};

QJsonObject member(const QString& session, const QString& role, const QString& number,
                   const QString& provider, bool idle = true) {
    return {{"session_id", session}, {"role", role},         {"name", session},
            {"number", number},      {"provider", provider}, {"idle", idle}};
}

// A Codex team of four; Developer 2 runs a turn.
QJsonObject codexTeam(const QString& policy = QStringLiteral("ask")) {
    return {{"id", "7"},
            {"name", "Shop"},
            {"repository", "/repo"},
            {"execution_policy", policy},
            {"members", QJsonArray{member("lead", "leader", "11", "claude"),
                                   member("dev1", "developer", "12", "codex"),
                                   member("dev2", "developer", "13", "codex", false),
                                   member("rev", "reviewer", "14", "claude")}}};
}

} // namespace

class TeamProviderDialogTest final : public QObject {
    Q_OBJECT

  private:
    FakeDaemon daemon_;
    std::unique_ptr<TeamDirectory> teams_;

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        daemon_ = {};
        teams_ = std::make_unique<TeamDirectory>();
        teams_->setTransport(daemon_.transport());
        teams_->setAvailable(true);
    }

    // The provider is a select of the whole table, company first; one not set up is listed
    // disabled. Model and effort sit side by side under it. Away from Codex, Claude is first.
    void everyProviderIsListedAndTheSwitchIsAway() {
        TeamProviderForm form(teams_.get(), codexTeam(), models);
        form.resize(520, form.sizeHint().height());
        form.show();
        QVERIFY(QTest::qWaitForWindowExposed(&form));
        ket::Select* providers = form.chooser()->providers();
        QStringList labels;
        for (int index = 0; index < providers->count(); ++index)
            labels.append(providers->itemText(index));
        QCOMPARE(labels, (QStringList{"OpenAI – Codex", "Anthropic – Claude", "Anysphere – Cursor",
                                      "GitHub – Copilot"}));
        const auto* items = qobject_cast<QStandardItemModel*>(providers->model());
        QVERIFY(items->item(providers->findData("codex"))->isEnabled());
        QVERIFY(!items->item(providers->findData("cursor"))->isEnabled());
        // The provider's name, not a number, says what to set up.
        QCOMPARE(items->item(providers->findData("copilot"))->toolTip(),
                 QStringLiteral("Set up GitHub Copilot in Settings → Agents first."));
        QCOMPARE(providers->currentData().toString(), QStringLiteral("claude"));
        QCOMPARE(form.chooser()->agent().model, QStringLiteral("opus"));
        const QPoint model = form.chooser()->modelSelect()->mapTo(&form, QPoint{});
        const QPoint effort =
            form.chooser()->findChild<QWidget*>("teamAgentEffort")->mapTo(&form, QPoint{});
        QCOMPARE(effort.y(), model.y());
        QVERIFY(effort.x() > model.x());
        QVERIFY(providers->width() > form.width() * 0.8);
    }

    // Every member is checked; the running one says it switches when its turn ends. The whole
    // team sends no member list, so the provider becomes the team's own.
    void theWholeTeamIsTheDefault() {
        TeamProviderForm form(teams_.get(), codexTeam(), models);
        QCOMPARE(form.chosen(), (QStringList{"lead", "dev1", "dev2", "rev"}));
        QVERIFY(form.allMembers()->isChecked());
        QVERIFY(form.member("dev2")->description().contains("switches when it ends"));
        QVERIFY(form.member("dev1")->description().contains("Codex"));
        QVERIFY(form.member("dev1")->text().contains("#12"));
        const QJsonObject payload = form.payload();
        QCOMPARE(payload.value("provider").toString(), QStringLiteral("claude"));
        QCOMPARE(payload.value("model").toString(), QStringLiteral("opus"));
        QCOMPARE(payload.value("effort").toString(), QStringLiteral("medium"));
        QVERIFY(!payload.contains("members"));
        auto* summary = form.findChild<ket::Banner*>("teamProviderSummary");
        QVERIFY(summary->message().contains("The one running now switches"));
    }

    // A member left out is not sent. All checks everyone again from a partial choice, and a
    // second press clears everyone, when nothing can be sent.
    void aMemberCanBeLeftOut() {
        TeamProviderForm form(teams_.get(), codexTeam(), models);
        form.member("lead")->setChecked(false);
        QVERIFY(!form.allMembers()->isChecked());
        QCOMPARE(form.payload().value("members").toArray(),
                 QJsonArray::fromStringList({"dev1", "dev2", "rev"}));
        form.allMembers()->click();
        QCOMPARE(form.chosen().size(), 4);
        form.allMembers()->click();
        QVERIFY(form.chosen().isEmpty());
        form.submit();
        QVERIFY(daemon_.sent.isEmpty());
    }

    // ketplus#295: without the team's Full access a reviewer stays where it is on a move to
    // Codex, and the dialog says why; the user may give the team Full access with the switch,
    // acknowledged as Create does, and the reviewer moves too.
    void aCodexReviewerNeedsFullAccess() {
        TeamProviderForm form(teams_.get(), codexTeam(), models);
        auto* access = form.findChild<ket::Banner*>("teamProviderAccess");
        form.show();
        QVERIFY(QTest::qWaitForWindowExposed(&form));
        QVERIFY(!access->isVisible());
        QVERIFY(!form.grantAccess()->isVisible());
        ket::Select* providers = form.chooser()->providers();
        providers->setCurrentIndex(providers->findData("codex"));
        QVERIFY(form.reviewerNeedsAccess());
        QVERIFY(access->isVisible());
        QVERIFY(form.grantAccess()->isVisible());
        QVERIFY(!form.grantAccess()->isChecked());
        QVERIFY(form.findChild<ket::Banner*>("teamProviderSummary")
                    ->message()
                    .contains("The reviewer stays on its provider."));
        QCOMPARE(form.payload().value("members").toArray(),
                 QJsonArray::fromStringList({"lead", "dev1", "dev2"}));
        QVERIFY(!form.payload().contains("execution_policy"));

        form.grantAccess()->setChecked(true);
        QVERIFY(form.grantsFullAccess());
        const QJsonObject granted = form.payload();
        QVERIFY(!granted.contains("members"));
        QCOMPARE(granted.value("execution_policy").toString(), QStringLiteral("full"));
        QCOMPARE(granted.value("risk_acknowledged").toBool(), true);

        // Without the reviewer chosen, nothing asks for Full access.
        form.member("rev")->setChecked(false);
        QVERIFY(!form.reviewerNeedsAccess());
        QVERIFY(!form.grantAccess()->isVisible());
        QVERIFY(!form.payload().contains("execution_policy"));

        TeamProviderForm full(teams_.get(), codexTeam("full"), models);
        full.chooser()->providers()->setCurrentIndex(
            full.chooser()->providers()->findData("codex"));
        QVERIFY(!full.reviewerNeedsAccess());
        QVERIFY(!full.payload().contains("members"));
        QVERIFY(!full.payload().contains("execution_policy"));
    }

    // The daemon's results replace the form: who switches when, who already ran on it, and
    // who could not move and why.
    void eachMembersResultIsShown() {
        daemon_.answers.insert(
            "team.provider.set",
            {{"ok", true},
             {"results", QJsonArray{QJsonObject{{"session_id", "lead"}, {"status", "unchanged"}},
                                    QJsonObject{{"session_id", "dev1"},
                                                {"status", "pending"},
                                                {"applies", "next_turn"}},
                                    QJsonObject{{"session_id", "dev2"},
                                                {"status", "pending"},
                                                {"applies", "after_current_turn"}},
                                    QJsonObject{{"session_id", "rev"},
                                                {"status", "skipped"},
                                                {"error_code", "context_switch_unsupported"},
                                                {"reason", "It moved to another worktree."}}}}});
        TeamProviderForm form(teams_.get(), codexTeam(), models);
        bool sent = false;
        form.submit([&] { sent = true; });
        QVERIFY(sent);
        QCOMPARE(daemon_.sent.size(), 1);
        QCOMPARE(daemon_.sent.first().first, QStringLiteral("team.provider.set"));
        QCOMPARE(daemon_.sent.first().second.value("team_id").toString(), QStringLiteral("7"));
        QVERIFY(form.showsResults());
        const auto line = [&](const QString& session) {
            auto* row = form.findChild<QWidget*>("teamProviderResult_" + session);
            return row ? row->findChild<team_page::Text*>("teamProviderResultLine")->text()
                       : QString{};
        };
        QCOMPARE(line("lead"), QStringLiteral("Already on Claude"));
        QCOMPARE(line("dev1"), QStringLiteral("Switches to Claude at its next turn"));
        QCOMPARE(line("dev2"), QStringLiteral("Switches to Claude when its current turn ends"));
        QCOMPARE(line("rev"), QStringLiteral("Not switched: It moved to another worktree."));
        QVERIFY(form.findChild<ket::Banner*>("teamProviderDone")->title().contains("2 members"));
    }

    // A refusal of the whole change stays in the form with the daemon's reason.
    void aRefusalStaysOpenWithTheReason() {
        daemon_.answers.insert("team.provider.set",
                               {{"ok", false},
                                {"error_code", "provider_not_ready"},
                                {"error_message", "Claude is not ready: it is signed out."}});
        TeamProviderForm form(teams_.get(), codexTeam(), models);
        form.submit();
        QVERIFY(!form.showsResults());
        QCOMPARE(form.error(), QStringLiteral("Claude is not ready: it is signed out."));
    }

    // The dialog: Switch counts the members, then turns into Done once the results show; it
    // fits the window and follows the theme and every zoom step.
    void theDialogSwitchesAndThenOnlyCloses_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<qreal>("zoom");
        for (const bool dark : {false, true})
            for (const qreal zoom : {1.0, 1.25, 1.5})
                QTest::newRow(
                    qPrintable(QStringLiteral("%1-%2")
                                   .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
                                   .arg(qRound(zoom * 100))))
                    << dark << zoom;
    }
    void theDialogSwitchesAndThenOnlyCloses() {
        QFETCH(bool, dark);
        QFETCH(qreal, zoom);
        ThemeManager theme(*qApp);
        theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
        ket::Appearance::instance().setZoom(zoom);
        QWidget window;
        window.resize(1024, 640);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        ket::Dialog* dialog = TeamProviderForm::present(&window, teams_.get(), codexTeam(), models);
        auto* form = dialog->findChild<TeamProviderForm*>();
        auto* send = dialog->findChild<ket::Button*>("teamProviderSwitch");
        QCOMPARE(send->text(), QStringLiteral("Switch all 4 members"));
        form->member("lead")->setChecked(false);
        QCOMPARE(send->text(), QStringLiteral("Switch 3 members"));
        QTRY_VERIFY(dialog->card()->geometry().bottom() < window.height());
        QVERIFY(dialog->card()->geometry().left() >= 0);
        // The theme switches live while the dialog is open: it keeps its choices and still fits.
        theme.setMode(dark ? ThemeManager::Mode::Light : ThemeManager::Mode::Dark);
        QCoreApplication::processEvents();
        QCOMPARE(send->text(), QStringLiteral("Switch 3 members"));
        QTRY_VERIFY(dialog->card()->geometry().bottom() < window.height());
        QTest::mouseClick(send, Qt::LeftButton);
        QVERIFY(form->showsResults());
        QVERIFY(!send->isVisible());
        ket::Appearance::instance().setZoom(1.0);
        dialog->reject();
    }
};

QTEST_MAIN(TeamProviderDialogTest)
#include "TeamProviderDialogTest.moc"
