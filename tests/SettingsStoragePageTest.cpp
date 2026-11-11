#include "app/SettingsStoragePage.h"
#include "KetBanner.h"
#include "KetMeter.h"
#include "app/SettingsDialog.h"
#include "app/SettingsWorkspacePage.h"
#include "ui/Theme.h"
#include <QAbstractButton>
#include <QAccessible>
#include <QComboBox>
#include <QDir>
#include <QJsonArray>
#include <QLabel>
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
QJsonObject supportedState(bool options = true) {
    auto result = state();
    result.insert("size_policy_supported", true);
    if (options)
        result.insert("storage_options_supported", true);
    return result;
}
QJsonObject optionsState(qint64 revision = 0, int archived = 90, const QString& images = "preview",
                         bool warn = true) {
    return {{"ok", true},
            {"revision", revision},
            {"options", QJsonObject{{"archived_history_days", archived},
                                    {"generated_images", images},
                                    {"warn_near_limit", warn}}}};
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
// The mock's journal: 9.0 GB against a 5 GB limit, 4 GB free on disk.
QJsonObject measuredSize() {
    auto size = sizeState();
    size.insert("usage", QJsonObject{{"database_page_bytes", 9'000'000'000LL},
                                     {"free_page_bytes", 5'200'000'000LL},
                                     {"kept_page_bytes", 3'800'000'000LL}});
    size.insert("disk_usage", QJsonObject{{"database_file_bytes", 9'000'000'000LL},
                                          {"wal_file_bytes", 100'000'000LL},
                                          {"shared_memory_file_bytes", 32768},
                                          {"total_file_bytes", 9'100'032'768LL},
                                          {"filesystem_available_bytes", 4'000'000'000LL}});
    size.insert("status", QJsonObject{{"target_scope", "sqlite_pages"},
                                      {"database_page_bytes", 9'000'000'000LL},
                                      {"limit_bytes", 5'000'000'000LL},
                                      {"over_limit", true},
                                      {"warning", true},
                                      {"low_disk", true},
                                      {"filesystem_available_bytes", 4'000'000'000LL},
                                      {"pressure_enabled", true}});
    return size;
}
QJsonObject measuredBreakdown() {
    return {{"ok", true},
            {"state", "ready"},
            {"breakdown",
             QJsonObject{{"database_page_bytes", 9'000'000'000LL},
                         {"parts", QJsonObject{{"history_bytes", 2'300'000'000LL},
                                               {"image_bytes", 1'200'000'000LL},
                                               {"image_count", 422},
                                               {"activity_bytes", 300'000'000LL},
                                               {"free_bytes", 5'200'000'000LL}}},
                         {"largest", QJsonArray{QJsonObject{{"session_id", "s"},
                                                            {"number", "5498"},
                                                            {"title", "Port the image pipeline"},
                                                            {"provider", "codex"},
                                                            {"bytes", 349'000'000LL},
                                                            {"note", "318 generated images"}}}},
                         {"directory", QDir::homePath() + "/Library/KetPlus/daemon"},
                         {"oldest_event_at", "2026-09-24T09:00:00.000Z"}}}};
}
// A current daemon: every storage area answers; `overrides` replaces single operations.
SettingsStoragePage::Requester daemon(QStringList* calls = nullptr,
                                      QHash<QString, QJsonObject> overrides = {},
                                      QJsonObject* payload = nullptr) {
    return [calls, overrides, payload](const QString& op, const QJsonObject& data, auto reply) {
        if (calls)
            calls->append(op);
        if (payload && op.endsWith("apply"))
            *payload = data;
        if (overrides.contains(op))
            reply(overrides.value(op), {});
        else if (op == "read" || op == "apply")
            reply(supportedState(), {});
        else if (op == "size.read" || op == "size.apply")
            reply(sizeState(), {});
        else if (op.startsWith("options."))
            reply(optionsState(), {});
        else if (op == "breakdown.read")
            reply({{"ok", true}, {"state", "ready"}}, {});
        else
            reply({{"ok", true}}, {});
    };
}
QComboBox* days(SettingsStoragePage& page) {
    return page.findChild<QComboBox*>("storageDetailDays");
}
QAbstractButton* automatic(SettingsStoragePage& page) {
    return page.findChild<QAbstractButton*>("storageAutomaticDetail");
}
QLabel* status(SettingsStoragePage& page) { return page.findChild<QLabel*>("storageStatus"); }
QAbstractButton* reload(SettingsStoragePage& page) {
    return page.findChild<QAbstractButton*>("storageReload");
}
void choose(SettingsStoragePage& page, int value) {
    days(page)->setCurrentIndex(days(page)->findData(value));
    emit days(page)->activated(days(page)->currentIndex());
}
} // namespace
class SettingsStoragePageTest final : public QObject {
    Q_OBJECT
  private slots:
    void sizeAppliesImmediatelyWithItsRevision() {
        QStringList calls;
        QJsonObject submitted;
        auto saved = sizeState(1, 2'000'000'000LL, 90);
        SettingsStoragePage page(daemon(&calls, {{"size.apply", saved}}, &submitted));
        auto* limit = page.findChild<QComboBox*>("storageSizeLimit");
        QTRY_VERIFY(limit->isEnabled());
        QCOMPARE(calls, QStringList({"read", "size.read", "options.read", "breakdown.read"}));
        QVERIFY(page.findChild<QAbstractButton*>("storageSizeSave") == nullptr);
        QCOMPARE(limit->currentData().toLongLong(), 5'000'000'000LL);
        limit->setCurrentIndex(limit->findData(2'000'000'000LL));
        emit limit->activated(limit->currentIndex());
        QCOMPARE(calls.at(4), QString("size.apply"));
        QCOMPARE(submitted.value("expected_revision").toInteger(), 0);
        // The warning threshold has no control; it keeps the saved value.
        QCOMPARE(submitted.value("policy").toObject(),
                 (QJsonObject{{"limit_bytes", 2'000'000'000LL}, {"warning_percent", 80}}));
        QTRY_VERIFY(status(page)->text().startsWith("Saved"));
        QCOMPARE(page.findChild<QAbstractButton*>("storageWarnNearLimit")->text(),
                 QString("Warn me when the journal passes 90% of its limit"));
        QVERIFY(page.findChild<QLabel*>("storageSizeStatus")->text().contains("90%"));
        // A reply that does not confirm the choice requires a reload.
        SettingsStoragePage stale(daemon(nullptr, {{"size.apply", sizeState(1)}}));
        auto* other = stale.findChild<QComboBox*>("storageSizeLimit");
        QTRY_VERIFY(other->isEnabled());
        other->setCurrentIndex(other->findData(qint64(0)));
        emit other->activated(other->currentIndex());
        QVERIFY(!other->isEnabled());
        QVERIFY(!reload(stale)->isHidden());
    }
    void optionsApplyImmediatelyAndOlderDaemonsHideThem() {
        QStringList calls;
        QJsonObject submitted;
        SettingsStoragePage page(
            daemon(&calls, {{"options.apply", optionsState(1, 365, "caption", true)}}, &submitted));
        auto* images = page.findChild<QComboBox*>("storageGeneratedImages");
        auto* archived = page.findChild<QComboBox*>("storageArchivedHistory");
        auto* warn = page.findChild<QAbstractButton*>("storageWarnNearLimit");
        QTRY_VERIFY(images->isEnabled());
        QCOMPARE(images->currentData().toString(), QString("preview"));
        QCOMPARE(archived->currentData().toInt(), 90);
        QVERIFY(warn->isChecked());
        archived->setCurrentIndex(archived->findData(365));
        images->setCurrentIndex(images->findData("caption"));
        emit images->activated(images->currentIndex());
        QCOMPARE(calls.last(), QString("options.apply"));
        QCOMPARE(submitted.value("options").toObject(),
                 (QJsonObject{{"archived_history_days", 365},
                              {"generated_images", "caption"},
                              {"warn_near_limit", true}}));
        QVERIFY(status(page)->text().startsWith("Saved"));
        QVERIFY(images->isEnabled());
        // A conflict requires a reload; nothing is applied on top of the other window's save.
        SettingsStoragePage conflict(daemon(
            nullptr, {{"options.apply", {{"ok", false}, {"error_code", "policy_conflict"}}}}));
        auto* toggle = conflict.findChild<QAbstractButton*>("storageWarnNearLimit");
        QTRY_VERIFY(toggle->isEnabled());
        toggle->click();
        QVERIFY(!toggle->isEnabled());
        QVERIFY(status(conflict)->text().contains("another window"));
        // A daemon without the options keeps the rest of the page.
        SettingsStoragePage older(daemon(nullptr, {{"read", supportedState(false)}}));
        QTRY_VERIFY(older.findChild<QComboBox*>("storageSizeLimit")->isEnabled());
        QVERIFY(older.findChild<QComboBox*>("storageGeneratedImages")->isHidden() ||
                !older.findChild<QComboBox*>("storageGeneratedImages")->isVisibleTo(&older));
        QVERIFY(!older.findChild<QAbstractButton*>("storageExport")->isEnabled());
    }
    void overviewShowsTheMeasuredJournal() {
        const auto size = measuredSize();
        const auto breakdown = measuredBreakdown();
        auto read = supportedState();
        read.insert("policy",
                    QJsonObject{{"full_detail_days", 0}, {"automatic_detail_retention", true}});
        SettingsStoragePage page(
            daemon(nullptr, {{"read", read}, {"size.read", size}, {"breakdown.read", breakdown}}));
        QTRY_VERIFY(days(page)->isEnabled());
        QCOMPARE(page.findChild<QLabel*>("storageFileSize")->text(), QString("9.1 GB"));
        QCOMPARE(page.findChild<QLabel*>("storageDiskAvailable")->text(), QString("4.0 GB"));
        auto* banner = page.findChild<ket::Banner*>("storageLimitBanner");
        QVERIFY(banner->isVisibleTo(&page));
        QCOMPARE(banner->title(), QString("The journal is over its size limit"));
        QCOMPARE(banner->message(), QString("9.0 GB on disk, limit 5 GB. 5.2 GB of it is free "
                                            "space that compacting returns to macOS."));
        QVERIFY(page.findChild<ket::Banner*>("storageDiskBanner")->isVisibleTo(&page));
        auto* journal = page.findChild<ket::Meter*>("storageJournalMeter");
        QCOMPARE(journal->valueText(), QString("9.0 GB of 5 GB"));
        QCOMPARE(journal->fraction(), 1.0);
        QVERIFY(journal->caption().startsWith("~/Library/KetPlus/daemon · full detail since "));
        QVERIFY(journal->caption().contains("2026"));
        const auto parts = page.findChildren<ket::Meter*>("storagePartMeter");
        QCOMPARE(parts.size(), 4);
        QCOMPARE(parts[0]->valueText(), QString("2.3 GB"));
        QCOMPARE(parts[1]->caption(), QString("422 images"));
        QCOMPARE(parts[3]->valueText(), QString("5.2 GB"));
        const auto rows = page.findChildren<QLabel*>("storageLargestConversation");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first()->text(),
                 QString("Port the image pipeline · Codex · 349 MB · 318 generated images"));
    }
    void compactNowShowsProgressAndReloads() {
        QStringList calls;
        QJsonObject compaction{{"state", "running"},
                               {"before_bytes", 9'000'000'000LL},
                               {"database_bytes", 6'100'000'000LL},
                               {"sweeps", 1},
                               {"sweep_progress", 0.5}};
        auto overrides = QHash<QString, QJsonObject>{
            {"compaction.start", {{"ok", true}, {"compaction", compaction}}}};
        auto requester = daemon(&calls, overrides);
        QJsonObject finished = compaction;
        bool done = false;
        SettingsStoragePage page([&](const QString& op, const QJsonObject& data, auto reply) {
            if (op == "compaction.read" && done) {
                calls.append(op);
                reply({{"ok", true}, {"compaction", finished}}, {});
                return;
            }
            if (op == "compaction.read") {
                calls.append(op);
                reply({{"ok", true}, {"compaction", compaction}}, {});
                return;
            }
            requester(op, data, reply);
        });
        auto* compact = page.findChild<QAbstractButton*>("storageCompactNow");
        QTRY_VERIFY(compact->isEnabled());
        compact->click();
        QCOMPARE(calls.last(), QString("compaction.start"));
        auto* progress = page.findChild<ket::Meter*>("storageCompactionProgress");
        QVERIFY(progress->isVisibleTo(&page));
        QCOMPARE(progress->valueText(), QString("9.0 GB → 6.1 GB"));
        QCOMPARE(progress->fraction(), 0.5);
        QCOMPARE(progress->caption(), QString("Pass 2"));
        QVERIFY(!compact->isEnabled());
        auto* poll = page.findChild<QTimer*>("storageJobsPoll");
        QVERIFY(poll->isActive());
        done = true;
        finished.insert("state", "idle");
        finished.insert("outcome", "done");
        finished.insert("database_bytes", 3'800'000'000LL);
        finished.insert("last", QJsonObject{{"completed_at", "2026-11-10T08:00:00.000Z"},
                                            {"before_bytes", 9'000'000'000LL},
                                            {"after_bytes", 3'800'000'000LL}});
        calls.clear();
        QVERIFY(QMetaObject::invokeMethod(poll, "timeout", Qt::DirectConnection));
        QCOMPARE(page.findChild<QLabel*>("storageJobStatus")->text(),
                 QString("Journal compacted · 9.0 GB → 3.8 GB"));
        QVERIFY(!progress->isVisibleTo(&page));
        QVERIFY(
            page.findChild<QLabel*>("storageLastCompaction")->text().endsWith("9.0 GB → 3.8 GB"));
        QVERIFY(calls.contains("read")); // the sizes, the warning and the parts are read again
        QTRY_VERIFY(compact->isEnabled());
    }
    void exportUsesTheChosenFolder() {
        QStringList calls;
        QJsonObject submitted;
        QJsonObject exportStatus{{"state", "running"}, {"exported", 12}, {"total", 88}};
        auto requester = daemon(&calls);
        SettingsStoragePage page([&](const QString& op, const QJsonObject& data, auto reply) {
            if (op.startsWith("export.")) {
                calls.append(op);
                if (op == "export.start")
                    submitted = data;
                reply({{"ok", true}, {"export", exportStatus}}, {});
                return;
            }
            requester(op, data, reply);
        });
        QString folder;
        page.setFolderChooser([&](QWidget*) { return folder; });
        auto* button = page.findChild<QAbstractButton*>("storageExport");
        QTRY_VERIFY(button->isEnabled());
        button->click(); // cancelled
        QVERIFY(!calls.contains("export.start"));
        folder = "/tmp/exports";
        button->click();
        QCOMPARE(submitted, (QJsonObject{{"directory", "/tmp/exports"}}));
        auto* text = page.findChild<QLabel*>("storageJobStatus");
        QCOMPARE(text->text(), QString("Exporting 12 of 88 conversations…"));
        QVERIFY(!button->isEnabled());
        exportStatus = {{"state", "done"},
                        {"exported", 88},
                        {"directory", "/tmp/exports/KetPlus conversations"}};
        QVERIFY(QMetaObject::invokeMethod(page.findChild<QTimer*>("storageJobsPoll"), "timeout",
                                          Qt::DirectConnection));
        QCOMPARE(text->text(), QString("Exported 88 conversations to /tmp/exports/KetPlus "
                                       "conversations"));
        QVERIFY(button->isEnabled());
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
        QCOMPARE(page.findChild<QLabel*>("storageFileSize")->text(), QString("Loading…"));
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
        auto* page = new SettingsStoragePage(daemon());
        scroll.setWidget(page);
        scroll.setWidgetResizable(true);
        scroll.resize(width, 700);
        scroll.show();
        auto* limit = page->findChild<QComboBox*>("storageSizeLimit");
        auto* images = page->findChild<QComboBox*>("storageGeneratedImages");
        auto* compact = page->findChild<QAbstractButton*>("storageCompactNow");
        QTRY_VERIFY(limit->isEnabled());
        scroll.ensureWidgetVisible(compact);
        QTRY_VERIFY(scroll.viewport()->rect().contains(
            compact->mapTo(scroll.viewport(), compact->rect().bottomRight())));
        QVERIFY(page->width() <= width);
        for (auto* text : page->findChildren<QLabel*>()) {
            if (text->isVisible() && text->wordWrap())
                QTRY_VERIFY(text->height() >= text->heightForWidth(text->width()));
        }
        // Each control sits at the row's end at the mock's 200 px.
        QCOMPARE(limit->width(), 200);
        limit->setFocus();
        QTest::keyClick(limit, Qt::Key_Tab);
        QVERIFY(images->hasFocus());
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
        SettingsWorkspacePage shell(
            dialog, {}, nullptr, {},
            [modern](const QString& op, const QJsonObject& data, auto reply) {
                if (modern) {
                    daemon(nullptr, {{"size.read", measuredSize()},
                                     {"breakdown.read", measuredBreakdown()}})(op, data, reply);
                    return;
                }
                reply(lowDiskState(), {});
            });
        shell.resize(1100, 760);
        shell.show();
        shell.showSection("storage");
        auto* page = shell.findChild<SettingsStoragePage*>();
        QVERIFY(page);
        QTRY_VERIFY(page->isVisible());
        QTRY_VERIFY(days(*page)->isEnabled());
        auto* compact = page->findChild<QAbstractButton*>("storageCompactNow");
        QTRY_VERIFY(compact->isVisible());
        QVERIFY(!reload(*page)->isVisible()); // only after a failure
        QVERIFY(shell.findChild<QWidget*>("settingsWorkspaceFooter")->isHidden());
        auto* scroll = qobject_cast<QScrollArea*>(page->parentWidget()->parentWidget());
        QVERIFY(scroll);
        const QString capture = qEnvironmentVariable("KETPLUS_STORAGE_CAPTURE");
        const QString name = QString(modern ? "size-" : "") + (dark ? "dark" : "light");
        if (!capture.isEmpty())
            QVERIFY(shell.grab().save(capture + name + "-top.png"));
        scroll->ensureWidgetVisible(compact);
        QTRY_VERIFY(shell.rect().contains(compact->mapTo(&shell, compact->rect().bottomRight())));
        if (!capture.isEmpty())
            QVERIFY(shell.grab().save(capture + name + ".png"));
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
        page.resize(width, 1400);
        page.show();
        QTRY_VERIFY(days(page)->isEnabled());
        auto* files = page.findChild<QLabel*>("storageFileSize");
        QCOMPARE(files->text(), QString("3.5 GB"));
        QCOMPARE(page.findChild<QLabel*>("storageDiskAvailable")->text(), QString("4.0 GB"));
        QVERIFY(page.width() <= width);
        const auto inside = [&](QWidget* widget) {
            return page.rect().contains(QRect(widget->mapTo(&page, QPoint()), widget->size()));
        };
        QVERIFY(inside(files));
        QVERIFY(inside(days(page)));
        QVERIFY(inside(automatic(page)));
        QVERIFY(inside(page.findChild<QAbstractButton*>("storageExport")));
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
