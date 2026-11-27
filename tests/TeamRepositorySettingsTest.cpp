// T-90 (ADR 0123): the team's Repository settings send the repository, the integration branch
// and the reason, keep the daemon's refusal in its words, and the team page names the whole
// path. Against a fake daemon; run with QT_QPA_PLATFORM=offscreen.
#include "teams/TeamRepositorySettings.h"
#include "KetBanner.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetProfile.h"
#include "KetStyle.h"
#include "KetTextField.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPage.h"

#include <QApplication>
#include <QJsonArray>
#include <QLabel>
#include <QPointer>
#include <QStandardPaths>
#include <QtTest>

using namespace ketplus;

namespace {

QJsonObject team() {
    return {{"id", "7"},
            {"name", "Shop"},
            {"goal", "Checkout"},
            {"repository", "/work/legacy-shop"},
            {"integration_branch", "integration"},
            {"merge_policy", "leader"},
            {"turn_budget", 60},
            {"automatic_turns", 0},
            {"max_rounds", 2},
            {"review_place", "board"},
            {"members",
             QJsonArray{QJsonObject{{"session_id", "lead"}, {"role", "leader"}, {"name", "Lead"}}}},
            {"tasks", QJsonArray{}},
            {"decisions", QJsonArray{}},
            {"revision", 1}};
}

ket::TextField* field(QWidget* root, const char* name) {
    return root->findChild<ket::TextField*>(QLatin1String(name));
}

} // namespace

class TeamRepositorySettingsTest final : public QObject {
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
                answers_.value(type, {{"ok", true}, {"team", QJsonObject{{"id", "7"}}}});
            reply(answer, answer.value("ok").toBool() ? QString{}
                                                      : answer.value("error_message").toString());
        });
        teams_->setAvailable(true);
    }

    void theTeamPageNamesTheWholePath() {
        TeamPage page(teams_.get(), "7", "/work/legacy-shop");
        page.resize(1280, 900);
        page.show();
        QTRY_VERIFY(page.findChild<QLabel*>("teamPageRepository"));
        auto* label = page.findChild<QLabel*>("teamPageRepository");
        QCOMPARE(label->text(), QStringLiteral("legacy-shop"));
        QCOMPARE(label->parentWidget()->toolTip(), QStringLiteral("/work/legacy-shop"));
    }

    void theSettingsSendTheRepositoryAndKeepARefusal() {
        TeamPage page(teams_.get(), "7", "/work/legacy-shop");
        page.show();
        QPointer<ket::Dialog> dialog = teamRepositorySettings(&page, teams_.get(), team());
        QCOMPARE(field(dialog, "teamRepositoryPath")->text(), QStringLiteral("/work/legacy-shop"));
        QCOMPARE(field(dialog, "teamRepositoryBranch")->text(), QStringLiteral("integration"));
        // Nothing changed: Save closes without a request.
        dialog->findChild<ket::Button*>("teamRepositorySave")->click();
        QTRY_VERIFY(!dialog);
        QVERIFY(lastOf("team.update").isEmpty());

        // The daemon's refusal stays in the dialog, in its words.
        answers_.insert("team.update",
                        {{"ok", false},
                         {"error_code", "invalid_repository"},
                         {"error_message", "origin has no branch main in /work/shop. Fetch it "
                                           "(git fetch origin main) or name another integration "
                                           "branch."}});
        dialog = teamRepositorySettings(&page, teams_.get(), team());
        field(dialog, "teamRepositoryPath")->setText(" /work/shop ");
        field(dialog, "teamRepositoryBranch")->setText("main");
        field(dialog, "teamRepositoryReason")->setText("The legacy repository is archived.");
        dialog->findChild<ket::Button*>("teamRepositorySave")->click();
        const QJsonObject update = lastOf("team.update");
        QCOMPARE(update.value("team_id").toString(), QStringLiteral("7"));
        QCOMPARE(update.value("repository").toString(), QStringLiteral("/work/shop"));
        QCOMPARE(update.value("integration_branch").toString(), QStringLiteral("main"));
        QCOMPARE(update.value("reason").toString(),
                 QStringLiteral("The legacy repository is archived."));
        auto* refusal = dialog->findChild<ket::Banner*>("teamRepositoryRefusal");
        QTRY_VERIFY(!refusal->isHidden());
        QVERIFY(refusal->message().contains("origin has no branch main"));
        QVERIFY(dialog->isVisible());

        // Taken, the dialog closes. Without a reason none is sent.
        answers_.remove("team.update");
        field(dialog, "teamRepositoryReason")->setText(" ");
        dialog->findChild<ket::Button*>("teamRepositorySave")->click();
        QVERIFY(!lastOf("team.update").contains("reason"));
        QTRY_VERIFY(!dialog);
    }
};

QTEST_MAIN(TeamRepositorySettingsTest)
#include "TeamRepositorySettingsTest.moc"
