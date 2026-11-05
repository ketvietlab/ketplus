#include "app/SettingsClaudeAnalytics.h"
#include "app/SettingsClaudeCredentials.h"
#include "app/SettingsDialog.h"
#include "app/SettingsWorkspacePage.h"
#include "ui/Theme.h"
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest>

using ketplus::SettingsClaudeCredentials;
class ClaudeCredentialsSettingsTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("KetPlusTests");
        QCoreApplication::setApplicationName("ClaudeCredentialsSettingsTest");
    }
    void composedSettingsFitAndRenderInBothThemes() {
        ketplus::ThemeManager theme(*qApp);
        auto* dialog = new ketplus::SettingsDialog(ketplus::AppearanceSettings{}, theme);
        ketplus::SettingsWorkspacePage page(
            dialog, [](auto reply) { reply({}, {}); }, nullptr,
            [](const QString&, const QJsonObject&, auto reply) {
                reply({{"configured", true},
                       {"enabled", true},
                       {"supported", true},
                       {"budget_usd", 2.5},
                       {"observation", QJsonObject{{"source", "ANTHROPIC_API_KEY"},
                                                   {"estimated_cost_usd", 0.1534}}}},
                      {});
            });
        page.showSection("providers");
        const auto evidence = qEnvironmentVariable("KETPLUS_BYOK_EVIDENCE_DIR");
        for (const bool dark : {false, true}) {
            theme.previewMode(dark ? ketplus::ThemeManager::Mode::Dark
                                   : ketplus::ThemeManager::Mode::Light);
            page.applyPalette(theme.palette());
            page.resize(dark ? 960 : 1220, 900);
            page.show();
            auto* key = page.findChild<QLineEdit*>("claudeApiKey");
            QTRY_VERIFY(key && key->isVisible());
            QTRY_VERIFY(page.findChild<QPushButton*>("claudeCredentialsSave")->isEnabled());
            const auto* form = page.findChild<SettingsClaudeCredentials*>();
            for (auto* widget : form->findChildren<QWidget*>()) {
                if (!widget->isVisible())
                    continue;
                const QRect box(widget->mapTo(form, QPoint()), widget->size());
                QVERIFY2(box.left() >= 0 && box.right() < form->width(),
                         qPrintable(widget->objectName()));
            }
            if (!evidence.isEmpty()) {
                QVERIFY(QDir().mkpath(evidence));
                QVERIFY(page.grab().save(
                    QDir(evidence).filePath(dark ? "providers-dark.png" : "providers-light.png")));
            }
        }
    }
    void adminCredentialUsesSeparateOperationAndNeverEchoesSecret() {
        QString operation;
        QJsonObject payload;
        SettingsClaudeCredentials::Reply pending;
        ketplus::SettingsClaudeAnalytics page(
            [&](const QString& op, const QJsonObject& p, auto reply) {
                operation = op;
                payload = p;
                if (op == "analytics.status")
                    reply({{"configured", false}, {"supported", true}}, {});
                else
                    pending = std::move(reply);
            });
        page.show();
        QTRY_COMPARE(operation, "analytics.status");
        auto* key = page.findChild<QLineEdit*>("claudeAdminKey");
        auto* save = page.findChild<QPushButton*>("claudeAdminSave");
        auto* refresh = page.findChild<QPushButton*>("claudeAdminRefresh");
        QVERIFY(key && save && refresh);
        QCOMPARE(key->echoMode(), QLineEdit::Password);
        key->setText("sk-ant-admin01-test-only");
        QTest::mouseClick(save, Qt::LeftButton);
        QCOMPARE(operation, "analytics.save");
        QCOMPARE(payload.value("key").toString(), "sk-ant-admin01-test-only");
        QVERIFY(key->text().isEmpty());
        QVERIFY(!save->isEnabled());
        pending({{"configured", true}, {"supported", true}}, {});
        QVERIFY(save->isEnabled());
        QTest::mouseClick(refresh, Qt::LeftButton);
        QCOMPARE(operation, "analytics.refresh");
        QVERIFY(payload.isEmpty());
        pending({}, "Admin access denied");
        auto* status = page.findChild<QLabel*>("claudeAdminStatus");
        QVERIFY(status);
        QCOMPARE(status->text(), "Admin access denied");
        QVERIFY(save->isEnabled());
    }
    void savesSecretWithoutDisplayingItAndChecksConnection() {
        QJsonObject state{
            {"configured", false}, {"enabled", false}, {"supported", true}, {"budget_usd", 0}};
        QString operation;
        QJsonObject last;
        SettingsClaudeCredentials::Reply pending;
        SettingsClaudeCredentials page(
            [&](const QString& op, const QJsonObject& payload, auto reply) {
                operation = op;
                last = payload;
                if (op == "status")
                    reply(state, {});
                else
                    pending = std::move(reply);
            });
        page.resize(680, 480);
        page.show();
        QSignalSpy changed(&page, &SettingsClaudeCredentials::refreshed);
        QTRY_COMPARE(operation, QString("status"));
        auto* key = page.findChild<QLineEdit*>("claudeApiKey");
        auto* mode = page.findChild<QComboBox*>("claudeAuthenticationMode");
        auto* budget = page.findChild<QDoubleSpinBox*>("claudeQueryBudget");
        auto* save = page.findChild<QPushButton*>("claudeCredentialsSave");
        auto* check = page.findChild<QPushButton*>("claudeCredentialsCheck");
        QVERIFY(key && mode && budget && save && check);
        QCOMPARE(key->echoMode(), QLineEdit::Password);
        QVERIFY(!check->isEnabled());
        mode->setCurrentIndex(1);
        key->setText("sk-ant-ui-fixture");
        budget->setValue(2.5);
        QTest::mouseClick(save, Qt::LeftButton);
        QCOMPARE(operation, "save");
        QCOMPARE(last["key"].toString(), "sk-ant-ui-fixture");
        QCOMPARE(last["budget_usd"].toDouble(), 2.5);
        QVERIFY(key->text().isEmpty());
        QVERIFY(!save->isEnabled());
        state["configured"] = true;
        state["enabled"] = true;
        state["budget_usd"] = 2.5;
        pending(state, {});
        QVERIFY(check->isEnabled());
        QTest::mouseClick(check, Qt::LeftButton);
        QCOMPARE(operation, "check");
        QVERIFY(last.isEmpty());
        QVERIFY(!check->isEnabled());
        pending({}, "Anthropic rejected this API key.");
        QVERIFY(page.findChild<QLabel*>("claudeCredentialsStatus")->text().contains("rejected"));
        QVERIFY(check->isEnabled());
        for (auto* label : page.findChildren<QLabel*>())
            QVERIFY(!label->text().contains("sk-ant-"));
        for (auto* widget : QList<QWidget*>{key, mode, budget, save, check}) {
            QVERIFY(widget->isVisible());
            QVERIFY(page.rect().contains(QRect(widget->mapTo(&page, QPoint()), widget->size())));
        }
        const auto screenshot = qEnvironmentVariable("KETPLUS_BYOK_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(page.grab().save(screenshot));
    }
    void recoversFromUnavailableDaemonAndProtectsDestroyedPage() {
        SettingsClaudeCredentials::Reply pending;
        auto* page = new SettingsClaudeCredentials(
            [&](const QString&, const QJsonObject&, auto reply) { pending = std::move(reply); });
        page->show();
        QTRY_VERIFY(bool(pending));
        pending({}, "Connect to an updated local KetPlus daemon.");
        QVERIFY(!page->findChild<QPushButton*>("claudeCredentialsSave")->isEnabled());
        QVERIFY(page->findChild<QPushButton*>("claudeCredentialsRefresh")->isEnabled());
        page->refresh();
        auto late = pending;
        delete page;
        late({{"supported", true}}, {});
    }
};
QTEST_MAIN(ClaudeCredentialsSettingsTest)
#include "ClaudeCredentialsSettingsTest.moc"
