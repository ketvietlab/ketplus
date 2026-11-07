#include "app/SettingsStoragePage.h"
#include "app/SettingsDialog.h"
#include "app/SettingsWorkspacePage.h"
#include "ui/Theme.h"
#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QtTest>
using namespace ketplus;
namespace {
QJsonObject state(qint64 revision = 0, int days = 30, bool automatic = true) {
    return {{"ok", true},
            {"revision", revision},
            {"policy",
             QJsonObject{{"full_detail_days", days}, {"automatic_detail_retention", automatic}}},
            {"usage", QJsonObject{{"database_page_bytes", 4096},
                                  {"free_page_bytes", 1024},
                                  {"kept_page_bytes", 3072}}}};
}
QComboBox* days(SettingsStoragePage& page) {
    return page.findChild<QComboBox*>("storageDetailDays");
}
QCheckBox* automatic(SettingsStoragePage& page) {
    return page.findChild<QCheckBox*>("storageAutomaticDetail");
}
QLabel* status(SettingsStoragePage& page) { return page.findChild<QLabel*>("storageStatus"); }
QPushButton* reload(SettingsStoragePage& page) {
    return page.findChild<QPushButton*>("storageReload");
}
void choose(SettingsStoragePage& page, int value) {
    days(page)->setCurrentIndex(days(page)->findData(value));
    emit days(page)->activated(days(page)->currentIndex());
}
} // namespace
class SettingsStoragePageTest final : public QObject {
    Q_OBJECT
  private slots:
    void loadsAndSavesWithRevisionWithoutMutatingOnRead() {
        QList<QString> operations;
        QJsonObject payload;
        SettingsStoragePage::Reply pending;
        SettingsStoragePage page([&](const QString& op, const QJsonObject& data, auto reply) {
            operations.append(op);
            payload = data;
            pending = std::move(reply);
        });
        QVERIFY(!days(page)->isEnabled());
        QTRY_COMPARE(operations.size(), 1);
        QCOMPARE(operations.first(), "read");
        pending(state(3), {});
        QVERIFY(days(page)->isEnabled());
        QCOMPARE(days(page)->currentData().toInt(), 30);
        QCOMPARE(operations.size(), 1);
        choose(page, 0);
        QCOMPARE(operations.last(), "apply");
        QCOMPARE(payload.value("expected_revision").toInteger(), 3);
        QCOMPARE(payload.value("policy").toObject().value("full_detail_days").toInt(), 0);
        QVERIFY(!reload(page)->isEnabled());
        QVERIFY(!automatic(page)->isEnabled());
        QVERIFY(!status(page)->text().contains("Saved"));
        pending(state(4, 0), {});
        QVERIFY(status(page)->text().startsWith("Saved"));
        automatic(page)->click();
        QCOMPARE(payload.value("expected_revision").toInteger(), 4);
        QVERIFY(!payload.value("policy").toObject().value("automatic_detail_retention").toBool());
        pending(state(5, 0, false), {});
        QCOMPARE(operations.size(), 3);
    }
    void failedOrConflictingSaveRequiresReload_data() {
        QTest::addColumn<QString>("error");
        QTest::addColumn<QJsonObject>("response");
        QTest::newRow("disconnect") << QString("Daemon disconnected") << QJsonObject{};
        QTest::newRow("conflict") << QString{}
                                  << QJsonObject{{"ok", false}, {"error_code", "policy_conflict"}};
        QTest::newRow("storage-error")
            << QString{} << QJsonObject{{"ok", false}, {"error_message", "disk full"}};
    }
    void failedOrConflictingSaveRequiresReload() {
        QFETCH(QString, error);
        QFETCH(QJsonObject, response);
        int reads = 0, writes = 0;
        SettingsStoragePage page([&](const QString& op, const QJsonObject&, auto reply) {
            if (op == "read") {
                ++reads;
                reply(state(reads, reads == 1 ? 30 : 90), {});
            } else {
                ++writes;
                reply(response, error);
            }
        });
        QTRY_VERIFY(days(page)->isEnabled());
        choose(page, 7);
        QCOMPARE(writes, 1);
        QVERIFY(!days(page)->isEnabled());
        QVERIFY(!status(page)->text().contains("Saved"));
        QVERIFY(status(page)->text().contains("Reload"));
        reload(page)->click();
        QCOMPARE(reads, 2);
        QCOMPARE(days(page)->currentData().toInt(), 90);
        QVERIFY(days(page)->isEnabled());
        QCOMPARE(writes, 1);
    }
    void malformedResponseDoesNotEnableDestructiveChoices_data() {
        QTest::addColumn<QJsonObject>("response");
        auto value = state();
        value["revision"] = 0.5;
        QTest::newRow("fractional") << value;
        value = state();
        value["revision"] = -1;
        QTest::newRow("negative") << value;
        value = state();
        value["policy"] =
            QJsonObject{{"full_detail_days", 1}, {"automatic_detail_retention", true}};
        QTest::newRow("unknown-days") << value;
        value = state();
        value["policy"] =
            QJsonObject{{"full_detail_days", 30}, {"automatic_detail_retention", "true"}};
        QTest::newRow("string-bool") << value;
        value = state();
        value.remove("policy");
        QTest::newRow("missing") << value;
    }
    void malformedResponseDoesNotEnableDestructiveChoices() {
        QFETCH(QJsonObject, response);
        SettingsStoragePage page(
            [&](const QString&, const QJsonObject&, auto reply) { reply(response, {}); });
        QTRY_VERIFY(reload(page)->isEnabled());
        QVERIFY(!days(page)->isEnabled());
        QVERIFY(!automatic(page)->isEnabled());
        QVERIFY(status(page)->text().contains("unsupported"));
    }
    void mismatchedSaveAcknowledgmentRequiresReload_data() {
        QTest::addColumn<QJsonObject>("response");
        QTest::newRow("old-revision") << state(0, 7);
        QTest::newRow("other-choice") << state(1, 90);
        QTest::newRow("other-toggle") << state(1, 7, false);
    }
    void mismatchedSaveAcknowledgmentRequiresReload() {
        QFETCH(QJsonObject, response);
        SettingsStoragePage page([&](const QString& op, const QJsonObject&, auto reply) {
            reply(op == "read" ? state() : response, {});
        });
        QTRY_VERIFY(days(page)->isEnabled());
        choose(page, 7);
        QVERIFY(!days(page)->isEnabled());
        QVERIFY(!status(page)->text().startsWith("Saved"));
        QVERIFY(status(page)->text().contains("Reload"));
    }
    void metricsFailureDoesNotTurnCommittedSaveIntoFailure() {
        SettingsStoragePage page([](const QString& op, const QJsonObject&, auto reply) {
            auto result = state(op == "read" ? 0 : 1, op == "read" ? 30 : 7);
            result.remove("usage");
            result["usage_error"] = "busy";
            reply(result, {});
        });
        QTRY_VERIFY(days(page)->isEnabled());
        choose(page, 7);
        QVERIFY(status(page)->text().startsWith("Saved"));
        QVERIFY(page.findChild<QLabel*>("storageUsage")->text().contains("unavailable"));
        QVERIFY(days(page)->isEnabled());
    }
    void timeoutRequiresReloadAndIgnoresLateSave() {
        QList<SettingsStoragePage::Reply> replies;
        SettingsStoragePage page(
            [&](const QString&, const QJsonObject&, auto reply) { replies.append(reply); });
        QTRY_COMPARE(replies.size(), 1);
        replies[0](state(1), {});
        choose(page, 7);
        QCOMPARE(replies.size(), 2);
        auto* timer = page.findChild<QTimer*>("storageRequestTimeout");
        QVERIFY(timer && timer->isActive());
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QVERIFY(!timer->isActive());
        QVERIFY(!days(page)->isEnabled());
        reload(page)->click();
        QCOMPARE(replies.size(), 3);
        replies[2](state(3, 90), {});
        replies[1](state(2, 7), {});
        QCOMPARE(days(page)->currentData().toInt(), 90);
        QVERIFY(!status(page)->text().startsWith("Saved"));
    }
    void unavailableAndLateRepliesAreSafe() {
        SettingsStoragePage unavailable;
        QTRY_VERIFY(reload(unavailable)->isEnabled());
        QVERIFY(!days(unavailable)->isEnabled());
        SettingsStoragePage::Reply pending;
        auto* page = new SettingsStoragePage(
            [&](const QString&, const QJsonObject&, auto reply) { pending = reply; });
        QTRY_VERIFY(bool(pending));
        delete page;
        pending(state(), {});
    }
    void nativeSettingsShell_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void nativeSettingsShell() {
        QFETCH(bool, dark);
        ThemeManager theme(*qApp);
        theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
        auto* dialog = new SettingsDialog(AppearanceSettings{}, theme);
        SettingsWorkspacePage shell(
            dialog, {}, nullptr, {},
            [](const QString&, const QJsonObject&, auto reply) { reply(state(), {}); });
        shell.applyPalette(theme.palette());
        shell.resize(1100, 760);
        shell.show();
        shell.showSection("storage");
        auto* page = shell.findChild<SettingsStoragePage*>();
        QVERIFY(page);
        QTRY_VERIFY(page->isVisible());
        QTRY_VERIFY(days(*page)->isEnabled());
        QTRY_VERIFY(reload(*page)->isVisible());
        QVERIFY(shell.findChild<QDialogButtonBox*>("settingsWorkspaceFooter")->isHidden());
        QVERIFY(shell.rect().contains(
            reload(*page)->mapTo(&shell, reload(*page)->rect().bottomRight())));
        const QString capture = qEnvironmentVariable("KETPLUS_STORAGE_CAPTURE");
        if (!capture.isEmpty())
            QVERIFY(shell.grab().save(capture + (dark ? "dark" : "light") + ".png"));
    }
    void layoutAndKeyboard_data() {
        QTest::addColumn<int>("width");
        QTest::newRow("narrow") << 440;
        QTest::newRow("wide") << 800;
    }
    void layoutAndKeyboard() {
        QFETCH(int, width);
        SettingsStoragePage page(
            [](const QString&, const QJsonObject&, auto reply) { reply(state(), {}); });
        page.resize(width, 650);
        page.show();
        QTRY_VERIFY(days(page)->isEnabled());
        QVERIFY(page.width() <= width);
        QVERIFY(page.rect().contains(days(page)->geometry()));
        QVERIFY(page.rect().contains(automatic(page)->geometry()));
        QVERIFY(page.rect().contains(reload(page)->geometry()));
        auto* accessible = QAccessible::queryAccessibleInterface(days(page));
        QVERIFY(accessible);
        QCOMPARE(accessible->text(QAccessible::Name), days(page)->currentText());
        QVERIFY(accessible->text(QAccessible::Description).contains("Keep full detail for"));
        days(page)->setFocus();
        QTest::keyClick(days(page), Qt::Key_Tab);
        QVERIFY(automatic(page)->hasFocus());
    }
};
QTEST_MAIN(SettingsStoragePageTest)
#include "SettingsStoragePageTest.moc"
