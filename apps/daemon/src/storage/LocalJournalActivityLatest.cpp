#include "storage/LocalJournal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>

// T-91: a running activity group's heading names the step it is on. The summary carries that
// step's latest item, trimmed to what a client needs to word it as its own row.

namespace ketplus::daemon {
namespace {

// Output and results are what make an item large, and no row title reads them.
const QStringList kOutputKeys{
    QStringLiteral("aggregatedOutput"),
    QStringLiteral("output"),
    QStringLiteral("formattedOutput"),
    QStringLiteral("result"),
    QStringLiteral("contentItems"),
    QStringLiteral("content"),
    QStringLiteral("stdout"),
    QStringLiteral("stderr"),
    QStringLiteral("diff"),
    QStringLiteral("unified_diff"),
};
constexpr int kTextLimit = 512;
constexpr int kListLimit = 20;
constexpr int kDepthLimit = 6;

QJsonValue trimmed(const QJsonValue& value, int depth);

QJsonObject trimmedObject(const QJsonObject& object, const int depth) {
    QJsonObject result;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (kOutputKeys.contains(it.key()))
            continue;
        result.insert(it.key(), trimmed(it.value(), depth + 1));
    }
    return result;
}

QJsonValue trimmed(const QJsonValue& value, const int depth) {
    if (depth > kDepthLimit)
        return QJsonValue{};
    if (value.isString()) {
        const QString text = value.toString();
        return text.size() > kTextLimit ? text.left(kTextLimit) : text;
    }
    if (value.isObject())
        return trimmedObject(value.toObject(), depth);
    if (value.isArray()) {
        QJsonArray result;
        const QJsonArray list = value.toArray();
        for (qsizetype index = 0; index < list.size() && index < kListLimit; ++index)
            result.append(trimmed(list.at(index), depth + 1));
        return result;
    }
    return value;
}

} // namespace

QJsonObject LocalJournal::activityGroupLatest(const QJsonObject& event, const qint64 group) const {
    // The group's last step: the member that joined it last and has not left it.
    QSqlQuery last(database_);
    last.prepare("SELECT item_id FROM activity_items WHERE session_id=? AND generation=? AND "
                 "group_cursor=? AND family<>'evicted' ORDER BY first_cursor DESC LIMIT 1");
    last.addBindValue(event.value("session_id").toString());
    last.addBindValue(event.value("session_generation").toInteger());
    last.addBindValue(group);
    if (!last.exec() || !last.next())
        return {};
    QSqlQuery state(database_);
    state.prepare("SELECT event_type,payload FROM replay_events WHERE session_id=? AND "
                  "generation=? AND item_id=? AND event_type IN "
                  "('item.started','item.updated','item.completed') ORDER BY cursor DESC LIMIT 1");
    state.addBindValue(event.value("session_id").toString());
    state.addBindValue(event.value("session_generation").toInteger());
    state.addBindValue(last.value(0).toString());
    if (!state.exec() || !state.next())
        return {};
    const auto item = QJsonDocument::fromJson(state.value(1).toString().toUtf8())
                          .object()
                          .value("item")
                          .toObject();
    if (item.isEmpty())
        return {};
    return {{"item", trimmedObject(item, 0)},
            {"completed", state.value(0).toString() == QStringLiteral("item.completed")}};
}

} // namespace ketplus::daemon
