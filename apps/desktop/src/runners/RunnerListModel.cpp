#include "runners/RunnerListModel.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

namespace ketplus::runners {
namespace {

bool isRunnerId(const QString& id) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    return pattern.match(id).hasMatch();
}

QString text(const QJsonObject& object, const QString& key) {
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString() : QString{};
}

// Only a JSON true counts: a missing flag, a number or a string is false.
bool flag(const QJsonObject& object, const QString& key) {
    const QJsonValue value = object.value(key);
    return value.isBool() && value.toBool();
}

QStringList labels(const QJsonObject& object) {
    QStringList list;
    for (const QJsonValue& value : object.value(QStringLiteral("labels")).toArray()) {
        if (value.isString() && !value.toString().isEmpty())
            list.append(value.toString());
    }
    return list;
}

std::optional<QDateTime> lastSeen(const QJsonObject& object) {
    const QString value = text(object, QStringLiteral("last_seen_at"));
    if (value.isEmpty())
        return std::nullopt;
    const QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!parsed.isValid())
        return std::nullopt;
    return parsed.toUTC();
}

Relation relation(const QJsonObject& object) {
    const bool pinned = flag(object, QStringLiteral("pinned"));
    const bool paired = flag(object, QStringLiteral("paired_with_this_machine"));
    const QJsonValue trusted = object.value(QStringLiteral("trusted"));
    if (pinned && paired)
        return trusted.isBool() && !trusted.toBool() ? Relation::Disputed : Relation::Trusted;
    if (pinned)
        return Relation::PinnedOnly;
    if (paired)
        return Relation::PairedOnly;
    return Relation::NotPaired;
}

} // namespace

bool RunnerRow::canDisable() const { return !removed() && state != QStringLiteral("disabled"); }

Unavailable unavailableFor(const QString& errorCode, const QString& error) {
    if (errorCode == QStringLiteral("not_enrolled"))
        return Unavailable::NotEnrolled;
    if (errorCode == QStringLiteral("runner_unavailable"))
        return Unavailable::RelayUnavailable;
    if (errorCode == QStringLiteral("entitlement_required"))
        return Unavailable::NoEntitlement;
    if (errorCode == QStringLiteral("network"))
        return Unavailable::Network;
    if (errorCode == QStringLiteral("unknown_operation"))
        return Unavailable::DaemonUnsupported;
    // The requester failed without a daemon answer: no code at all.
    if (errorCode.isEmpty() && !error.isEmpty())
        return Unavailable::DaemonUnreachable;
    return Unavailable::None;
}

Unavailable unavailableAfterStatus(const QJsonObject& status, const QString& error) {
    const QJsonValue configured = status.value(QStringLiteral("configured"));
    if (error.isEmpty() && status.value(QStringLiteral("ok")).toBool(false) &&
        configured.isBool() && !configured.toBool())
        return Unavailable::NotEnrolled;
    return Unavailable::RelayUnavailable;
}

RunnerList parseRunnerList(const QJsonObject& result, const QString& error) {
    RunnerList list;
    const bool ok = error.isEmpty() && result.value(QStringLiteral("ok")).toBool(false) &&
                    result.value(QStringLiteral("runners")).isArray();
    if (!ok) {
        const QString code = text(result, QStringLiteral("error_code"));
        list.unavailable = unavailableFor(code, error);
        if (list.unavailable == Unavailable::None) {
            list.errorCode = code.isEmpty() ? QStringLiteral("invalid_response") : code;
            list.errorMessage = text(result, QStringLiteral("error_message"));
        }
        return list;
    }
    QSet<QString> seen;
    for (const QJsonValue& value : result.value(QStringLiteral("runners")).toArray()) {
        const QJsonObject object = value.toObject();
        RunnerRow row;
        row.id = text(object, QStringLiteral("runner_id"));
        if (!isRunnerId(row.id) || seen.contains(row.id))
            continue;
        seen.insert(row.id);
        row.name = text(object, QStringLiteral("name"));
        row.os = text(object, QStringLiteral("os"));
        row.arch = text(object, QStringLiteral("arch"));
        row.labels = labels(object);
        row.state = text(object, QStringLiteral("state"));
        row.version = text(object, QStringLiteral("runner_version"));
        row.lastSeen = lastSeen(object);
        row.relation = relation(object);
        list.rows.append(row);
    }
    for (const QJsonValue& value : result.value(QStringLiteral("unlisted_pins")).toArray()) {
        if (value.isString() && isRunnerId(value.toString()))
            list.unlistedPins.append(value.toString());
    }
    const QJsonValue complete = result.value(QStringLiteral("complete"));
    list.complete = !complete.isBool() || complete.toBool();
    list.pinError = result.contains(QStringLiteral("pin_error"));
    return list;
}

} // namespace ketplus::runners
