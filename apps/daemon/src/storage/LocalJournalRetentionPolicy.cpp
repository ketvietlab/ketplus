#include "storage/LocalJournal.h"
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
namespace ketplus::daemon {
std::optional<JournalRetentionPolicy> LocalJournal::retentionPolicy() const {
    setError({});
    QSqlQuery query(database_);
    if (!query.exec("SELECT revision,policy,typeof(revision) FROM journal_retention_settings WHERE "
                    "singleton=1")) {
        setError(query.lastError().text());
        return std::nullopt;
    }
    if (!query.next()) {
        if (query.lastError().isValid()) {
            setError(query.lastError().text());
            return std::nullopt;
        }
        return JournalRetentionPolicy{};
    }
    auto result = JournalRetentionPolicy::fromJson(
        QJsonDocument::fromJson(query.value(1).toByteArray()).object());
    const auto revision = query.value(0).toLongLong();
    if (!result || query.value(2).toString() != "integer" || revision < 1 ||
        revision > 9007199254740991LL) {
        setError(
            "Stored detail-retention policy is invalid; automatic detail trimming is disabled.");
        return std::nullopt;
    }
    result->revision = revision;
    return result;
}
bool LocalJournal::setRetentionPolicy(const JournalRetentionPolicy& policy,
                                      qint64 expectedRevision) {
    setError({});
    if (!JournalRetentionPolicy::fromJson(policy.toJson()) || expectedRevision < 0 ||
        expectedRevision >= 9007199254740991LL) {
        setError("Invalid detail-retention policy or revision.");
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
    const auto current = retentionPolicy();
    if (!current)
        return fail(errorString());
    if (current->revision != expectedRevision)
        return fail("Detail-retention policy changed; reload before saving.");
    QSqlQuery query(database_);
    query.prepare(
        "INSERT INTO journal_retention_settings(singleton,revision,policy) VALUES(1,?,?) "
        "ON CONFLICT(singleton) DO UPDATE SET revision=excluded.revision,policy=excluded.policy "
        "WHERE journal_retention_settings.revision=?");
    query.addBindValue(expectedRevision + 1);
    query.addBindValue(
        QString::fromUtf8(QJsonDocument(policy.toJson()).toJson(QJsonDocument::Compact)));
    query.addBindValue(expectedRevision);
    if (!query.exec())
        return fail(query.lastError().text());
    if (query.numRowsAffected() != 1)
        return fail("Detail-retention policy changed; reload before saving.");
    query.finish();
    if (!database_.commit())
        return fail(database_.lastError().text());
    detailRetentionScanRow_ = 0; // new settings apply to a fresh bounded sweep
    return true;
}
std::optional<qint64> LocalJournal::pruneConfiguredReplayDetails(const QDateTime& now) {
    if (!now.isValid()) {
        setError("Invalid detail-retention time.");
        return std::nullopt;
    }
    const auto policy = retentionPolicy();
    if (!policy)
        return std::nullopt;
    const auto boundary = policy->cutoff(now);
    return boundary ? pruneReplayDetails(*boundary) : std::optional<qint64>(0);
}
} // namespace ketplus::daemon
