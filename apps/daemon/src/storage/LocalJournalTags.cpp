#include "storage/LocalJournal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

#include <array>

namespace ketplus::daemon {

bool LocalJournal::setSessionTags(const QString& sessionId, const qint64 generation,
                                  const QStringList& tags) {
    setError({});
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("UPDATE sessions SET tags = ? WHERE id = ? AND generation = ?"));
    query.addBindValue(QString::fromUtf8(
        QJsonDocument(QJsonArray::fromStringList(tags)).toJson(QJsonDocument::Compact)));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return query.numRowsAffected() == 1;
}

namespace {

struct DefaultTag {
    const char* label;
    const char* color;
};

// Until the user saves a catalog, the labels the mock's fixture offers.
constexpr std::array<DefaultTag, 7> defaultTags{{{"UI", "#4f6bed"},
                                                 {"Performance", "#c2780a"},
                                                 {"Review", "#7c5cd6"},
                                                 {"Atlas", "#3f8f7a"},
                                                 {"Account", "#2f8f4e"},
                                                 {"Bug", "#d1453b"},
                                                 {"Docs", "#5b6b7f"}}};

// One row, created on first use: the catalog is small and always read and written whole.
bool ensureTagCatalogTable(QSqlDatabase& database, QString* error) {
    QSqlQuery query(database);
    if (query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS tag_catalog ("
                                  "id INTEGER PRIMARY KEY CHECK (id = 1), "
                                  "tags TEXT NOT NULL)")))
        return true;
    *error = query.lastError().text();
    return false;
}

} // namespace

QJsonArray LocalJournal::tagCatalog() {
    setError({});
    QString error;
    QSqlQuery query(database_);
    if (!ensureTagCatalogTable(database_, &error))
        setError(error);
    else if (!query.exec(QStringLiteral("SELECT tags FROM tag_catalog WHERE id = 1")))
        setError(query.lastError().text());
    else if (query.next())
        return QJsonDocument::fromJson(query.value(0).toString().toUtf8()).array();
    QJsonArray defaults;
    for (const DefaultTag& tag : defaultTags)
        defaults.append(QJsonObject{{QStringLiteral("label"), QString::fromUtf8(tag.label)},
                                    {QStringLiteral("color"), QString::fromUtf8(tag.color)}});
    return defaults;
}

bool LocalJournal::setTagCatalog(const QJsonArray& catalog) {
    setError({});
    QString error;
    if (!ensureTagCatalogTable(database_, &error)) {
        setError(error);
        return false;
    }
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("INSERT INTO tag_catalog (id, tags) VALUES (1, ?) "
                                 "ON CONFLICT(id) DO UPDATE SET tags = excluded.tags"));
    query.addBindValue(QString::fromUtf8(QJsonDocument(catalog).toJson(QJsonDocument::Compact)));
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}

bool LocalJournal::setSessionAwaitingReply(const QString& sessionId, const qint64 generation,
                                           const bool awaiting) {
    setError({});
    QSqlQuery query(database_);
    query.prepare(
        QStringLiteral("UPDATE sessions SET awaiting_reply = ? WHERE id = ? AND generation = ?"));
    query.addBindValue(awaiting ? 1 : 0);
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return query.numRowsAffected() == 1;
}

} // namespace ketplus::daemon
