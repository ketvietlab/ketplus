#include "storage/LocalJournal.h"
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
namespace ketplus::daemon {
std::optional<JournalSizePolicy> LocalJournal::sizePolicy() const {
    setError({});
    QSqlQuery query(database_);
    if (!query.exec("SELECT revision,policy,typeof(revision) FROM journal_size_settings WHERE "
                    "singleton=1")) {
        setError(query.lastError().text());
        return std::nullopt;
    }
    if (!query.next()) {
        if (query.lastError().isValid()) {
            setError(query.lastError().text());
            return std::nullopt;
        }
        return JournalSizePolicy{};
    }
    auto result =
        JournalSizePolicy::fromJson(QJsonDocument::fromJson(query.value(1).toByteArray()).object());
    const auto revision = query.value(0).toLongLong();
    if (!result || query.value(2).toString() != "integer" || revision < 1 ||
        revision > 9007199254740991LL) {
        setError("Stored journal-size policy is invalid; size-pressure trimming is disabled.");
        return std::nullopt;
    }
    result->revision = revision;
    return result;
}
bool LocalJournal::setSizePolicy(const JournalSizePolicy& policy, qint64 expectedRevision) {
    setError({});
    if (!JournalSizePolicy::fromJson(policy.toJson()) || expectedRevision < 0 ||
        expectedRevision >= 9007199254740991LL) {
        setError("Invalid journal-size policy or revision.");
        return false;
    }
    if (!database_.transaction()) {
        setError(database_.lastError().text());
        return false;
    }
    const auto fail = [&](const QString& error) {
        database_.rollback();
        setError(error);
        return false;
    };
    const auto current = sizePolicy();
    if (!current)
        return fail(errorString());
    if (current->revision != expectedRevision)
        return fail("Journal-size policy changed; reload before saving.");
    QSqlQuery query(database_);
    query.prepare(
        "INSERT INTO journal_size_settings(singleton,revision,policy) VALUES(1,?,?) "
        "ON CONFLICT(singleton) DO UPDATE SET revision=excluded.revision,policy=excluded.policy "
        "WHERE journal_size_settings.revision=?");
    query.addBindValue(expectedRevision + 1);
    query.addBindValue(
        QString::fromUtf8(QJsonDocument(policy.toJson()).toJson(QJsonDocument::Compact)));
    query.addBindValue(expectedRevision);
    if (!query.exec())
        return fail(query.lastError().text());
    if (query.numRowsAffected() != 1)
        return fail("Journal-size policy changed; reload before saving.");
    query.finish();
    if (!database_.commit())
        return fail(database_.lastError().text());
    sizePressureRecent_ = false;
    sizePressureActive_ = false;
    detailRetentionScanRow_ = 0; // new settings apply to a fresh bounded sweep
    return true;
}
} // namespace ketplus::daemon
