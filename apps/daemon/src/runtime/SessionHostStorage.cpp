#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
namespace ketplus::daemon {
QJsonObject SessionHost::storageRetention(const QString& type, const QJsonObject& payload) {
    const auto fail = [](const QString& code, const QString& text) {
        return QJsonObject{{"ok", false}, {"error_code", code}, {"error_message", text}};
    };
    auto current = journal_->retentionPolicy();
    if (!current)
        return fail("invalid_stored_policy", journal_->errorString());
    if (type == "storage.retention.apply") {
        const auto proposed = JournalRetentionPolicy::fromJson(payload.value("policy").toObject());
        const auto expected = payload.value("expected_revision");
        if (payload.size() != 2 || !proposed || !expected.isDouble() || expected.toDouble(-1) < 0 ||
            expected.toDouble() >= 9007199254740991.0 ||
            expected.toDouble() != expected.toInteger(-1))
            return fail("invalid_policy", "Supply a valid policy and its expected revision.");
        if (expected.toInteger() != current->revision)
            return fail("policy_conflict",
                        "Detail-retention policy changed; reload before saving.");
        if (!journal_->setRetentionPolicy(*proposed, current->revision))
            return fail("journal_error", journal_->errorString());
        current = *proposed;
        current->revision = expected.toInteger() + 1;
    } else if (type != "storage.retention.read" || !payload.isEmpty()) {
        return fail("invalid_request", "Unsupported retention operation or parameters.");
    }
    QJsonObject result{
        {"ok", true}, {"revision", current->revision}, {"policy", current->toJson()}};
    result.insert("size_policy_supported", true);
    result.insert("storage_options_supported", true); // ADR 0108
    const auto space = journal_->space();
    // Statistics are advisory; a failed metrics read must not report a committed save as failed.
    if (space)
        result.insert("usage",
                      QJsonObject{{"database_page_bytes", space->pageSize * space->pageCount},
                                  {"free_page_bytes", space->freeBytes()},
                                  {"kept_page_bytes", space->keptBytes()}});
    else
        result.insert("usage_error", journal_->errorString());
    const auto disk = journal_->diskUsage();
    if (disk)
        result.insert("disk_usage", *disk);
    else
        result.insert("disk_usage_error", journal_->errorString());
    return result;
}
QJsonObject SessionHost::storageSize(const QString& type, const QJsonObject& payload) {
    const auto fail = [](const QString& code, const QString& text) {
        return QJsonObject{{"ok", false}, {"error_code", code}, {"error_message", text}};
    };
    auto current = journal_->sizePolicy();
    if (!current)
        return fail("invalid_stored_policy", journal_->errorString());
    if (type == "storage.size.apply") {
        const auto proposed = JournalSizePolicy::fromJson(payload.value("policy").toObject());
        const auto expected = payload.value("expected_revision");
        if (payload.size() != 2 || !proposed || !expected.isDouble() || expected.toDouble(-1) < 0 ||
            expected.toDouble() >= 9007199254740991.0 ||
            expected.toDouble() != expected.toInteger(-1))
            return fail("invalid_policy", "Supply a valid policy and its expected revision.");
        if (expected.toInteger() != current->revision)
            return fail("policy_conflict", "Journal-size policy changed; reload before saving.");
        if (!journal_->setSizePolicy(*proposed, current->revision))
            return fail("journal_error", journal_->errorString());
        current = *proposed;
        current->revision = expected.toInteger() + 1;
    } else if (type != "storage.size.read" || !payload.isEmpty()) {
        return fail("invalid_request", "Unsupported retention operation or parameters.");
    }
    QJsonObject result{
        {"ok", true}, {"revision", current->revision}, {"policy", current->toJson()}};
    const auto space = journal_->space();
    // Statistics are advisory; a failed metrics read must not report a committed save as failed.
    if (space)
        result.insert("usage",
                      QJsonObject{{"database_page_bytes", space->pageSize * space->pageCount},
                                  {"free_page_bytes", space->freeBytes()},
                                  {"kept_page_bytes", space->keptBytes()}});
    else
        result.insert("usage_error", journal_->errorString());
    if (space) {
        const auto retention = journal_->retentionPolicy();
        auto status = storageStatus();
        status.insert("target_scope", "sqlite_pages");
        status.insert("pressure_enabled", current->pressureConfigured() && retention &&
                                              retention->automaticDetailRetention &&
                                              retention->fullDetailDays > 0);
        result.insert("status", status);
        if (!retention)
            result.insert("retention_policy_error", journal_->errorString());
    }
    const auto disk = journal_->diskUsage();
    if (disk)
        result.insert("disk_usage", *disk);
    else
        result.insert("disk_usage_error", journal_->errorString());
    result.insert("compaction", storageJobs().compactionStatus());
    return result;
}
QJsonObject SessionHost::storageStatus() const {
    const auto space = journal_->space();
    const auto size = journal_->sizePolicy();
    if (!space || !size)
        return {};
    const auto options = journal_->storageOptions();
    const auto bytes = space->pageSize * space->pageCount;
    const bool over = size->limitBytes > 0 && bytes > size->limitBytes;
    const bool near =
        size->limitBytes > 0 && bytes >= size->limitBytes * size->warningPercent / 100;
    QJsonObject status{{"database_page_bytes", bytes},
                       {"limit_bytes", size->limitBytes},
                       {"over_limit", over},
                       // Over the limit always warns; the near-limit warning can be turned off.
                       {"warning", over || (near && (!options || options->warnNearLimit))}};
    // Journal writes fail on a full disk, so under 5 GB free is worth saying (ADR 0108).
    if (const auto disk = journal_->diskUsage();
        disk && disk->contains("filesystem_available_bytes")) {
        const auto available = disk->value("filesystem_available_bytes").toInteger(-1);
        if (available >= 0)
            status.insert("low_disk", available < lowDiskBytes);
        status.insert("filesystem_available_bytes", available);
    }
    return status;
}
JournalStorageJobs& SessionHost::storageJobs() {
    if (!storageJobs_)
        storageJobs_ = std::make_unique<JournalStorageJobs>(
            journal_, JournalStorageJobs::Hooks{
                          [this] { return hasActiveSessions(); },
                          [this](const QDateTime& now) { return expireArchivedSessions(now); }});
    return *storageJobs_;
}
QJsonObject SessionHost::storageOptions(const QString& type, const QJsonObject& payload) {
    const auto fail = [](const QString& code, const QString& text) {
        return QJsonObject{{"ok", false}, {"error_code", code}, {"error_message", text}};
    };
    auto current = journal_->storageOptions();
    if (!current)
        return fail("invalid_stored_policy", journal_->errorString());
    if (type == "storage.options.apply") {
        const auto proposed = JournalStorageOptions::fromJson(payload.value("options").toObject());
        const auto expected = payload.value("expected_revision");
        if (payload.size() != 2 || !proposed || !expected.isDouble() || expected.toDouble(-1) < 0 ||
            expected.toDouble() >= 9007199254740991.0 ||
            expected.toDouble() != expected.toInteger(-1))
            return fail("invalid_policy",
                        "Supply valid storage options and their expected revision.");
        if (expected.toInteger() != current->revision)
            return fail("policy_conflict", "Storage options changed; reload before saving.");
        if (!journal_->setStorageOptions(*proposed, current->revision))
            return fail("journal_error", journal_->errorString());
        current = *proposed;
        current->revision = expected.toInteger() + 1;
    } else if (type != "storage.options.read" || !payload.isEmpty()) {
        return fail("invalid_request", "Unsupported storage options operation or parameters.");
    }
    return {{"ok", true},
            {"revision", current->revision},
            {"options", current->toJson()},
            {"status", storageStatus()}};
}
} // namespace ketplus::daemon
