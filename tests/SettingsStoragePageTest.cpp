#include "app/SettingsStoragePage.h"
#include "app/SettingsDialog.h"
#include "app/SettingsWorkspacePage.h"
#include "ui/Theme.h"
#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
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
QJsonObject sizeState(qint64 revision = 0, qint64 limit = 5'000'000'000LL, int warning = 80) {
    auto result = state();
    result.insert("revision", revision);
    result.insert("policy", QJsonObject{{"limit_bytes", limit}, {"warning_percent", warning}});
    result.insert("status", QJsonObject{{"target_scope", "sqlite_pages"},
                                        {"over_limit", false},
                                        {"warning", true},
                                        {"pressure_enabled", revision > 0}});
    return result;
}
QJsonObject supportedState() {
    auto result = state();
    result.insert("size_policy_supported", true);
    return result;
}
QJsonObject lowDiskState() {
    auto result = state();
    result.insert("usage", QJsonObject{{"database_page_bytes", 3'000'000'000LL},
                                       {"free_page_bytes", 500'000'000LL},
                                       {"kept_page_bytes", 2'500'000'000LL}});
    result.insert("disk_usage", QJsonObject{{"database_file_bytes", 3'000'000'000LL},
                                            {"wal_file_bytes", 500'000'000LL},
                                            {"shared_memory_file_bytes", 32768},
                                            {"total_file_bytes", 3'500'032'768LL},
                                            {"filesystem_available_bytes", 4'000'000'000LL}});
    return result;
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
    void sizeDraftSurvivesDetailSave_data() {
        QTest::addColumn<bool>("conflict");
        QTest::newRow("preserved") << false;
        QTest::newRow("changed-revision") << true;
    }
    void sizeDraftSurvivesDetailSave() {
        QFETCH(bool, conflict);
        bool detailSaved = false;
        QJsonObject submitted;
        SettingsStoragePage page([&](const QString& op, const QJsonObject& payload, auto reply) {
            if (op == "read")
                reply(supportedState(), {});
            else if (op == "size.read")
                reply(sizeState(detailSaved && conflict ? 1 : 0), {});
            else if (op == "apply") {
                detailSaved = true;
                auto response = state(1, 90);
                response.insert("size_policy_supported", true);
                reply(response, {});
            } else {
                submitted = payload;
                reply(sizeState(1, 0, 95), {});
            }
        });
        auto* save = page.findChild<QPushButton*>("storageSizeSave");
        auto* limit = page.findChild<QComboBox*>("storageSizeLimit");
        auto* warning = page.findChild<QSpinBox*>("storageWarningPercent");
        QTRY_VERIFY(save->isEnabled());
        limit->setCurrentIndex(limit->findData(qint64(0)));
        warning->setValue(95);
        auto* draft = page.findChild<QLabel*>("storageSizeDraftStatus");
        QVERIFY(draft->text().contains("Unsaved"));
        choose(page, 90);
        QCOMPARE(limit->currentData().toLongLong(), qint64(0));
        QCOMPARE(warning->value(), 95);
        if (conflict) {
            QVERIFY(!save->isEnabled());
            QVERIFY(status(page)->text().contains("another window"));
            QVERIFY(submitted.isEmpty());
        } else {
            QVERIFY(save->isEnabled());
            save->click();
            QCOMPARE(submitted.value("expected_revision").toInteger(), 0);
            QCOMPARE(submitted.value("policy").toObject().value("limit_bytes").toInteger(),
                     qint64(0));
            QCOMPARE(submitted.value("policy").toObject().value("warning_percent").toInt(), 95);
            QVERIFY(draft->text().isEmpty());
        }
    }
    void sizeRequiresExplicitSaveAndKeepsIndependentRevision() {
        QStringList calls;
        QJsonObject submitted;
        SettingsStoragePage::Reply saveReply;
        SettingsStoragePage page([&](const QString& op, const QJsonObject& payload, auto reply) {
            calls << op;
            if (op == "read")
                reply(supportedState(), {});
            else if (op == "size.read")
                reply(sizeState(), {});
            else {
                submitted = payload;
                saveReply = reply;
            }
        });
        auto* button = page.findChild<QPushButton*>("storageSizeSave");
        auto* limit = page.findChild<QComboBox*>("storageSizeLimit");
        auto* warning = page.findChild<QSpinBox*>("storageWarningPercent");
        QTRY_VERIFY(button->isEnabled());
        QCOMPARE(calls, QStringList({"read", "size.read"}));
        auto* info = page.findChild<QLabel*>("storageSizeStatus");
        QVERIFY(info->text().contains("not been enabled"));
        limit->setCurrentIndex(limit->findData(qint64(2'000'000'000LL)));
        warning->setValue(90);
        QCOMPARE(calls.size(), 2); // Editing and opening never opt in.
        button->click();
        QCOMPARE(calls.last(), "size.apply");
        QCOMPARE(submitted.value("expected_revision").toInteger(), 0);
        QCOMPARE(submitted.value("policy").toObject(),
                 sizeState(1, 2'000'000'000LL, 90).value("policy").toObject());
        QVERIFY(!days(page)->isEnabled());
        QVERIFY(!button->isEnabled());
        saveReply(sizeState(1, 2'000'000'000LL, 90), {});
        QVERIFY(button->isEnabled());
        QVERIFY(status(page)->text().contains("saved"));
        QCOMPARE(QAccessible::queryAccessibleInterface(info)->text(QAccessible::Name),
                 info->text());
        choose(page, 90);
        QCOMPARE(submitted.value("expected_revision").toInteger(), 0); // Different policy revision.
    }
    void sizeFailuresRequireReload_data() {
        QTest::addColumn<QString>("failure");
        for (auto value : {"conflict", "revision", "policy", "timeout", "disconnect"})
            QTest::newRow(value) << QString(value);
    }
    void sizeFailuresRequireReload() {
        QFETCH(QString, failure);
        SettingsStoragePage::Reply pending;
        SettingsStoragePage page([&](const QString& op, const QJsonObject&, auto reply) {
            if (op == "read")
                reply(supportedState(), {});
            else if (op == "size.read")
                reply(sizeState(), {});
            else
                pending = reply;
        });
        auto* button = page.findChild<QPushButton*>("storageSizeSave");
        QTRY_VERIFY(button->isEnabled());
        button->click();
        QVERIFY(bool(pending));
        if (failure == "timeout")
            QVERIFY(QMetaObject::invokeMethod(page.findChild<QTimer*>("storageRequestTimeout"),
                                              "timeout", Qt::DirectConnection));
        else if (failure == "disconnect")
            pending({}, "Disconnected");
        else if (failure == "conflict")
            pending({{"ok", false}, {"error_code", "policy_conflict"}}, {});
        else if (failure == "revision")
            pending(sizeState(0), {});
        else
            pending(sizeState(1, 2'000'000'000LL), {});
        QVERIFY(!button->isEnabled());
        QVERIFY(!days(page)->isEnabled());
        QVERIFY(reload(page)->isEnabled());
        reload(page)->click();
        QVERIFY(button->isEnabled());
        pending(sizeState(1), {}); // stale callback after reload cannot overwrite it
        QVERIFY(page.findChild<QLabel*>("storageSizeStatus")->text().contains("not been enabled"));
    }
    void invalidSizePolicy_data() {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("fraction") << QString("warning_percent") << QJsonValue(20.5);
        QTest::newRow("zero") << QString("warning_percent") << QJsonValue(0);
        QTest::newRow("above-100") << QString("warning_percent") << QJsonValue(101);
        QTest::newRow("string") << QString("limit_bytes") << QJsonValue("5000000000");
        QTest::newRow("unsupported") << QString("limit_bytes") << QJsonValue(3'000'000'000LL);
        QTest::newRow("unknown") << QString("extra") << QJsonValue(true);
    }
    void invalidSizePolicy() {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        auto malformed = sizeState();
        auto policy = malformed.value("policy").toObject();
        policy.insert(field, value);
        malformed.insert("policy", policy);
        SettingsStoragePage page([&](const QString& op, const QJsonObject&, auto reply) {
            reply(op == "read" ? supportedState() : malformed, {});
        });
        QTRY_VERIFY(reload(page)->isEnabled());
        QVERIFY(!page.findChild<QPushButton*>("storageSizeSave")->isEnabled());
        QVERIFY(!days(page)->isEnabled());
    }
    void sizeReplyAfterDestruction() {
        SettingsStoragePage::Reply pending;
        auto* page =
            new SettingsStoragePage([&](const QString& op, const QJsonObject&, auto reply) {
                if (op == "read")
                    reply(supportedState(), {});
                else
                    pending = reply;
            });
        QTRY_VERIFY(bool(pending));
        delete page;
        pending(sizeState(), {});
    }
    void sizeUnavailableStatusAndOldDaemon() {
        bool modern = true;
        SettingsStoragePage page([&](const QString& op, const QJsonObject&, auto reply) {
            if (op == "read")
                reply(modern ? supportedState() : state(), {});
            else {
                auto value = sizeState(1);
                value.remove("status");
                value.remove("usage");
                reply(value, {});
            }
        });
        auto* button = page.findChild<QPushButton*>("storageSizeSave");
        QTRY_VERIFY(button->isEnabled());
        QVERIFY(page.findChild<QLabel*>("storageSizeStatus")->text().contains("unknown"));
        modern = false;
        reload(page)->click();
        QVERIFY(page.findChild<QWidget*>("storageSizeGroup")->isHidden());
        QVERIFY(!button->isEnabled());
        QVERIFY(days(page)->isEnabled());
    }
    void diskMeasurementsAndLowSpaceWarnings_data() {
        QTest::addColumn<QJsonValue>("available");
        QTest::addColumn<bool>("known");
        QTest::addColumn<bool>("low");
        QTest::newRow("below") << QJsonValue(4'999'999'999LL) << true << true;
        QTest::newRow("zero") << QJsonValue(0) << true << true;
        QTest::newRow("boundary") << QJsonValue(5'000'000'000LL) << true << false;
        QTest::newRow("above") << QJsonValue(6'000'000'000LL) << true << false;
        QTest::newRow("missing") << QJsonValue() << false << false;
        QTest::newRow("negative") << QJsonValue(-1) << false << false;
        QTest::newRow("fraction") << QJsonValue(2.5) << false << false;
        QTest::newRow("string") << QJsonValue("6000000000") << false << false;
    }
    void diskMeasurementsAndLowSpaceWarnings() {
        QFETCH(QJsonValue, available);
        QFETCH(bool, known);
        QFETCH(bool, low);
        auto replyValue = state();
        replyValue.insert("disk_usage", QJsonObject{{"database_file_bytes", 3000},
                                                    {"wal_file_bytes", 1000},
                                                    {"shared_memory_file_bytes", 1000},
                                                    {"total_file_bytes", 5000},
                                                    {"filesystem_available_bytes", available}});
        SettingsStoragePage page(
            [&](const QString&, const QJsonObject&, auto reply) { reply(replyValue, {}); });
        QTRY_VERIFY(days(page)->isEnabled());
        auto* usage = page.findChild<QLabel*>("storageUsage");
        QVERIFY(usage->text().contains("SQLite files:"));
        QCOMPARE(usage->text().contains("Low disk space"), low);
        QCOMPARE(usage->text().contains("available is unknown"), !known);
        auto* accessible = QAccessible::queryAccessibleInterface(usage);
        QVERIFY(accessible);
        QCOMPARE(accessible->text(QAccessible::Name), usage->text());
        auto disk = replyValue.value("disk_usage").toObject();
        disk.insert("filesystem_available_bytes", 7'000'000'000LL);
        disk.insert("total_file_bytes", 1); // inconsistent file metrics do not invalidate settings
        replyValue.insert("disk_usage", disk);
        reload(page)->click();
        QVERIFY(days(page)->isEnabled());
        QVERIFY(!usage->text().contains("Low disk space"));
        QVERIFY(usage->text().contains("file sizes unavailable"));
    }
    void accessibleStatusTracksSaveAndRecoveryMessages() {
        QList<SettingsStoragePage::Reply> replies;
        SettingsStoragePage page(
            [&](const QString&, const QJsonObject&, auto reply) { replies.append(reply); });
        const auto accessibleText = [&] {
            auto* accessible = QAccessible::queryAccessibleInterface(status(page));
            return accessible ? accessible->text(QAccessible::Name) : QString{};
        };
        QTRY_COMPARE(replies.size(), 1);
        QCOMPARE(accessibleText(), status(page)->text());
        QVERIFY(accessibleText().contains("Loading"));
        replies[0](state(), {});
        QCOMPARE(accessibleText(), status(page)->text());
        choose(page, 7);
        QCOMPARE(accessibleText(), QString("Saving…"));
        replies[1](state(1, 7), {});
        QCOMPARE(accessibleText(), status(page)->text());
        QVERIFY(accessibleText().startsWith("Saved"));
        choose(page, 90);
        replies[2]({}, "disk full");
        QCOMPARE(accessibleText(), status(page)->text());
        QVERIFY(accessibleText().contains("disk full"));
        QVERIFY(accessibleText().contains("Reload"));
        reload(page)->click();
        QCOMPARE(accessibleText(), status(page)->text());
        replies[3](state(2, 90), {});
        choose(page, 30);
        replies[4]({{"ok", false}, {"error_code", "policy_conflict"}}, {});
        QCOMPARE(accessibleText(), status(page)->text());
        QVERIFY(accessibleText().contains("another window"));
        reload(page)->click();
        auto* timer = page.findChild<QTimer*>("storageRequestTimeout");
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QCOMPARE(accessibleText(), status(page)->text());
        QVERIFY(accessibleText().contains("did not respond"));
    }
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
    void sizeNativeLayout_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("width");
        QTest::newRow("light-narrow") << false << 440;
        QTest::newRow("dark-wide") << true << 800;
    }
    void sizeNativeLayout() {
        QFETCH(bool, dark);
        QFETCH(int, width);
        ThemeManager theme(*qApp);
        theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
        QScrollArea scroll;
        auto* page = new SettingsStoragePage([](const QString& op, const QJsonObject&, auto reply) {
            reply(op == "read" ? supportedState() : sizeState(), {});
        });
        scroll.setWidget(page);
        scroll.setWidgetResizable(true);
        scroll.resize(width, 700);
        scroll.show();
        auto* save = page->findChild<QPushButton*>("storageSizeSave");
        auto* limit = page->findChild<QComboBox*>("storageSizeLimit");
        auto* warning = page->findChild<QSpinBox*>("storageWarningPercent");
        QTRY_VERIFY(save->isEnabled());
        warning->setValue(95);
        QTRY_VERIFY(save->isVisible());
        scroll.ensureWidgetVisible(save);
        QTRY_VERIFY(scroll.viewport()->rect().contains(
            save->mapTo(scroll.viewport(), save->rect().bottomRight())));
        QVERIFY(page->width() <= width);
        for (auto* text : page->findChildren<QLabel*>()) {
            if (text->isVisible() && text->wordWrap())
                QTRY_VERIFY(text->height() >= text->heightForWidth(text->width()));
        }
        limit->setFocus();
        QTest::keyClick(limit, Qt::Key_Tab);
        QVERIFY(warning->hasFocus());
        QTest::keyClick(warning, Qt::Key_Tab);
        QVERIFY(save->hasFocus());
        const auto capture = qEnvironmentVariable("KETPLUS_SIZE_UI_CAPTURE");
        if (!capture.isEmpty())
            QVERIFY(scroll.grab().save(capture + (dark ? "dark" : "light") + ".png"));
    }
    void nativeSettingsShell_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("modern");
        QTest::newRow("light") << false << false;
        QTest::newRow("dark") << true << false;
        QTest::newRow("size-light") << false << true;
        QTest::newRow("size-dark") << true << true;
    }
    void nativeSettingsShell() {
        QFETCH(bool, dark);
        QFETCH(bool, modern);
        ThemeManager theme(*qApp);
        theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
        auto* dialog = new SettingsDialog(AppearanceSettings{}, theme);
        SettingsWorkspacePage shell(dialog, {}, nullptr, {},
                                    [modern](const QString& op, const QJsonObject&, auto reply) {
                                        if (modern && op == "size.read") {
                                            reply(sizeState(), {});
                                            return;
                                        }
                                        auto value = lowDiskState();
                                        if (modern)
                                            value.insert("size_policy_supported", true);
                                        reply(value, {});
                                    });
        shell.applyPalette(theme.palette());
        shell.resize(1100, 760);
        shell.show();
        shell.showSection("storage");
        auto* page = shell.findChild<SettingsStoragePage*>();
        QVERIFY(page);
        QTRY_VERIFY(page->isVisible());
        QTRY_VERIFY(days(*page)->isEnabled());
        QTRY_VERIFY(reload(*page)->isVisible());
        QVERIFY(shell.findChild<QWidget*>("settingsWorkspaceFooter")->isHidden());
        if (modern) {
            auto* scroll = qobject_cast<QScrollArea*>(page->parentWidget()->parentWidget());
            QVERIFY(scroll);
            scroll->ensureWidgetVisible(reload(*page));
        }
        QTRY_VERIFY(shell.rect().contains(
            reload(*page)->mapTo(&shell, reload(*page)->rect().bottomRight())));
        const QString capture = qEnvironmentVariable("KETPLUS_STORAGE_CAPTURE");
        if (!capture.isEmpty())
            QVERIFY(shell.grab().save(capture + (modern ? "size-" : "") +
                                      (dark ? "dark" : "light") + ".png"));
    }
    void layoutAndKeyboard_data() {
        QTest::addColumn<int>("width");
        QTest::newRow("narrow") << 440;
        QTest::newRow("wide") << 800;
    }
    void layoutAndKeyboard() {
        QFETCH(int, width);
        SettingsStoragePage page(
            [](const QString&, const QJsonObject&, auto reply) { reply(lowDiskState(), {}); });
        page.resize(width, 650);
        page.show();
        QTRY_VERIFY(days(page)->isEnabled());
        auto* usage = page.findChild<QLabel*>("storageUsage");
        QVERIFY(usage->text().contains("Low disk space"));
        QVERIFY(page.rect().contains(usage->geometry()));
        QTRY_VERIFY2(usage->height() >= usage->heightForWidth(usage->width()),
                     qPrintable(QString("actual=%1 required=%2 width=%3")
                                    .arg(usage->height())
                                    .arg(usage->heightForWidth(usage->width()))
                                    .arg(usage->width())));
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
