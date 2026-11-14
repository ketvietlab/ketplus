#include "app/SettingsAppearancePreview.h"
#include "app/SettingsClaudeAnalytics.h"
#include "app/SettingsClaudeCredentials.h"
#include "app/SettingsDialog.h"
#include "app/SettingsWorkspacePage.h"
#include "ui/Theme.h"

#include "KetAppearance.h"
#include "KetAvatar.h"
#include "KetBadge.h"
#include "KetThemeCard.h"
#include <QAbstractButton>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFontInfo>
#include <QLabel>
#include <QLineEdit>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QStackedWidget>
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
    // A card applies its theme at once; Escape closes without taking it back.
    void choosingAThemeAppliesIt() {
        ketplus::ThemeManager theme(*qApp);
        theme.setMode(ketplus::ThemeManager::Mode::Light);
        QVERIFY(!theme.palette().dark);
        ketplus::SettingsDialog dialog(ketplus::AppearanceSettings{}, theme);
        auto* dark = dialog.findChild<ket::ThemeCard*>("darkThemeCard");
        QVERIFY(dark);
        dark->click();
        QCOMPARE(theme.mode(), ketplus::ThemeManager::Mode::Dark);
        dialog.show();
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QCOMPARE(theme.mode(), ketplus::ThemeManager::Mode::Dark);
        QVERIFY(theme.palette().dark);
        QVERIFY(dark->isChecked());
    }
    // The workspace preview is drawn at the interface zoom, its badge included.
    void workspacePreviewFollowsTheZoom() {
        auto& appearance = ket::Appearance::instance();
        const qreal zoom = appearance.zoom();
        const auto restore = qScopeGuard([&] { appearance.setZoom(zoom); });
        appearance.setZoom(1.0);
        ketplus::WorkspacePreview preview;
        preview.resize(preview.sizeHint());
        preview.show();
        const int codePixels = preview.codeFont().pixelSize();
        const int height = preview.sizeHint().height();
        appearance.setZoom(16.0 / 14.0);
        QVERIFY(preview.codeFont().pixelSize() > codePixels);
        QVERIFY(preview.sizeHint().height() > height);
        // The badge takes its zoomed size at once, before any resize of the preview.
        auto* badge = preview.findChild<ket::Badge*>();
        QVERIFY(badge);
        QTRY_COMPARE(badge->size(), badge->sizeHint());
        preview.resize(preview.sizeHint());
        QVERIFY(preview.rect().contains(badge->geometry()));
    }
    void composedSettingsFitAndRenderInBothThemes() {
        ketplus::ThemeManager theme(*qApp);
        auto* dialog = new ketplus::SettingsDialog(ketplus::AppearanceSettings{}, theme);
        ketplus::SettingsWorkspacePage page(
            dialog, [](bool, auto reply) { reply({}, {}); }, nullptr,
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
            page.resize(dark ? 960 : 1220, 900);
            page.show();
            auto* key = page.findChild<QLineEdit*>("claudeApiKey");
            QTRY_VERIFY(key && key->isVisible());
            auto* save = page.findChild<QAbstractButton*>("claudeCredentialsSave");
            QVERIFY(save);
            QTRY_VERIFY(save->isEnabled());
            const auto* form = page.findChild<SettingsClaudeCredentials*>();
            QVERIFY(form);
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
        auto* save = page.findChild<QAbstractButton*>("claudeAdminSave");
        auto* refresh = page.findChild<QAbstractButton*>("claudeAdminRefresh");
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
        auto* save = page.findChild<QAbstractButton*>("claudeCredentialsSave");
        auto* check = page.findChild<QAbstractButton*>("claudeCredentialsCheck");
        QVERIFY(key && mode && budget && save && check);
        QCOMPARE(key->echoMode(), QLineEdit::Password);
        QVERIFY(!check->isEnabled());
        mode->setCurrentIndex(1);
        key->setText("sk-ant-ui-fixture");
        QCOMPARE(budget->text(), QString("No query limit"));
        // Any amount can be typed, not only presets.
        budget->selectAll();
        QTest::keyClicks(budget, "5");
        budget->interpretText();
        QCOMPARE(budget->value(), 5.0);
        QTest::mouseClick(save, Qt::LeftButton);
        QCOMPARE(operation, "save");
        QCOMPARE(last["key"].toString(), "sk-ant-ui-fixture");
        QCOMPARE(last["budget_usd"].toDouble(), 5.0);
        QVERIFY(key->text().isEmpty());
        QVERIFY(!save->isEnabled());
        state["configured"] = true;
        state["enabled"] = true;
        state["budget_usd"] = 2.5;
        pending(state, {});
        QCOMPARE(budget->value(), 2.5);
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
        QVERIFY(!page->findChild<QAbstractButton*>("claudeCredentialsSave")->isEnabled());
        QVERIFY(page->findChild<QAbstractButton*>("claudeCredentialsRefresh")->isEnabled());
        page->refresh();
        auto late = pending;
        delete page;
        late({{"supported", true}}, {});
    }
    // The bundled fonts read by their own name, and the preview's code uses the editor font.
    void settingsNameTheBundledFonts() {
        ketplus::ThemeManager theme(*qApp);
        auto* dialog = new ketplus::SettingsDialog(ketplus::AppearanceSettings{}, theme);
        ketplus::SettingsWorkspacePage page(dialog, [](bool, auto reply) { reply({}, {}); });
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto* family = page.findChild<QComboBox*>("editorFontFamily");
        QVERIFY(family);
        QCOMPARE(family->currentData().toString(), QStringLiteral("KET Sans Mono"));
        QCOMPARE(family->currentText(), QString::fromUtf8("KÉT Sans Mono"));
        QCOMPARE(dialog->settings().fontFamily, QStringLiteral("KET Sans Mono"));
        auto* code = page.findChild<ketplus::WorkspacePreview*>("settingsWorkspacePreview");
        QVERIFY(code);
        QCOMPARE(code->codeFamily(), QStringLiteral("KET Sans Mono"));
        QCOMPARE(QFontInfo(code->codeFont()).family(), QStringLiteral("KET Sans Mono"));
        // The mock's rows: a 200 px select at the end, and the font named in the help line.
        page.resize(1440, 900);
        page.showSection("typography");
        auto* interfaceFont = page.findChild<QComboBox*>("settingsInterfaceFont");
        QVERIFY(interfaceFont);
        QCOMPARE(interfaceFont->width(), family->width());
        QVERIFY(interfaceFont->width() < 260);
        QTRY_VERIFY(interfaceFont->mapTo(&page, QPoint(interfaceFont->width(), 0)).x() >
                    page.width() / 2 + 300);
        bool help = false;
        for (auto* label : page.findChildren<QLabel*>())
            help = help || label->text() ==
                               QString::fromUtf8("KetPlus uses KÉT Sans throughout the interface.");
        QVERIFY(help);
    }
    // Account & devices names the account the daemon reports (ADR 0109) and leaves signing
    // in and out to Connections.
    void accountPageShowsTheSignedInAccount() {
        ketplus::ThemeManager theme(*qApp);
        theme.previewMode(ketplus::ThemeManager::Mode::Light);
        auto* dialog = new ketplus::SettingsDialog(ketplus::AppearanceSettings{}, theme);
        ketplus::SettingsWorkspacePage page(dialog, [](bool, auto reply) { reply({}, {}); });
        QJsonObject status;
        std::function<void(const QJsonObject&)> pending;
        page.setAccountStatusReader(
            [&pending](std::function<void(const QJsonObject&)> reply) { pending = reply; });
        page.resize(1440, 900);
        page.show();
        page.showSection(QStringLiteral("account-devices"));
        auto* account = page.findChild<ketplus::SettingsAccountPage*>();
        QVERIFY(account && account->isVisible());
        const auto role = [account](const char* name) {
            for (auto* label : account->findChildren<QLabel*>())
                if (label->property("accountRole").toString() == QLatin1String(name))
                    return label;
            return static_cast<QLabel*>(nullptr);
        };
        auto* signOut = account->findChild<QAbstractButton*>("accountSignOut");
        auto* manage = account->findChild<QAbstractButton*>("accountManageConnections");
        QVERIFY(signOut && manage);
        // Nothing is claimed until the daemon answers.
        QCOMPARE(role("name")->text(), QStringLiteral("Checking your account…"));
        QVERIFY(!signOut->isVisible());
        QVERIFY(pending);
        pending({{"configured", true},
                 {"account", QJsonObject{{"display_name", "Minh Tran"}, {"username", "minh"}}}});
        QCOMPARE(role("name")->text(), QStringLiteral("Minh Tran"));
        QCOMPARE(role("meta")->text(), QStringLiteral("minh · ketPlus Pro"));
        QCOMPARE(account->findChild<ket::Avatar*>()->initials(), QStringLiteral("MT"));
        QVERIFY(signOut->isVisible());
        QCOMPARE(manage->text(), QStringLiteral("Manage connections"));
        QCOMPARE(role("name")->textFormat(), Qt::PlainText);
        QCOMPARE(role("meta")->textFormat(), Qt::PlainText);
        // The mock's relay meter is left out: the relay reports no usage.
        for (auto* label : account->findChildren<QLabel*>())
            QVERIFY2(!label->text().contains(QStringLiteral("Relay")), qPrintable(label->text()));
        if (const auto directory = qEnvironmentVariable("KETPLUS_SETTINGS_CAPTURE_DIR");
            !directory.isEmpty())
            QVERIFY(page.grab().save(QDir(directory).filePath("account-signed-in.png")));

        QSignalSpy manageSpy(&page, &ketplus::SettingsWorkspacePage::accountDevicesRequested);
        QSignalSpy signOutSpy(&page, &ketplus::SettingsWorkspacePage::accountSignOutRequested);
        signOut->click();
        QCOMPARE(signOutSpy.size(), 1);

        pending({{"configured", false}});
        QCOMPARE(role("name")->text(), QStringLiteral("Not signed in"));
        QVERIFY(!signOut->isVisible());
        QVERIFY(!account->findChild<ket::Avatar*>()->isVisible());
        QCOMPARE(manage->text(), QStringLiteral("Sign in…"));
        manage->click();
        QCOMPARE(manageSpy.size(), 1);
    }

    // Every section opens a page; with KETPLUS_SETTINGS_CAPTURE_DIR set, each is saved for the
    // side-by-side with the mock's settings/<state> captures.
    void everySectionOpensItsPage() {
        ketplus::ThemeManager theme(*qApp);
        theme.previewMode(ketplus::ThemeManager::Mode::Light);
        auto* dialog = new ketplus::SettingsDialog(ketplus::AppearanceSettings{}, theme);
        ketplus::SettingsWorkspacePage page(dialog, [](bool, auto reply) { reply({}, {}); });
        page.resize(1440, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto* pages = page.findChild<QStackedWidget*>("settingsWorkspacePages");
        QVERIFY(pages);
        const auto directory = qEnvironmentVariable("KETPLUS_SETTINGS_CAPTURE_DIR");
        QSet<QWidget*> seen;
        for (const auto* id : {"appearance", "typography", "account-devices", "browser", "computer",
                               "providers", "archived", "storage", "updates"}) {
            page.showSection(QString::fromLatin1(id));
            QCoreApplication::processEvents();
            QVERIFY2(pages->currentWidget(), id);
            seen.insert(pages->currentWidget());
            if (!directory.isEmpty())
                QVERIFY(
                    page.grab().save(QDir(directory).filePath(QStringLiteral("%1.png").arg(id))));
        }
        QVERIFY(seen.size() >= 7);
        // As in the mock, Restore defaults is not offered where it would reset nothing.
        for (const auto* id : {"account-devices", "storage", "updates"}) {
            page.showSection(QString::fromLatin1(id));
            QCoreApplication::processEvents();
            QVERIFY2(page.findChild<QWidget*>("settingsWorkspaceFooter")->isHidden(), id);
        }
    }
};
QTEST_MAIN(ClaudeCredentialsSettingsTest)
#include "ClaudeCredentialsSettingsTest.moc"
