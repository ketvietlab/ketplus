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
    const auto space = journal_->space();
    // Statistics are advisory; a failed metrics read must not report a committed save as failed.
    if (space)
        result.insert("usage",
                      QJsonObject{{"database_page_bytes", space->pageSize * space->pageCount},
                                  {"free_page_bytes", space->freeBytes()},
                                  {"kept_page_bytes", space->keptBytes()}});
    else
        result.insert("usage_error", journal_->errorString());
    return result;
}
} // namespace ketplus::daemon
