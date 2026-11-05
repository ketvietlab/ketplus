#include "runtime/SessionHost.h"

#include <QJsonArray>
#include <QStringList>

namespace ketplus::daemon {

namespace {

// Beyond this many distinct ignored methods, further ones share one bucket.
constexpr qsizetype maxIgnoredMethods = 128;

// Codex notifications the session host acts on, with the string fields each one needs. A dotted
// path names a field of a nested object. Ids and types must be non-empty; a delta may be empty.
// turn/completed and error carry no required field: a turn must always be able to finish.
const QHash<QString, QStringList>& handledNotifications() {
    static const QHash<QString, QStringList> fields{
        {QStringLiteral("thread/name/updated"), {}},
        {QStringLiteral("thread/tokenUsage/updated"), {}},
        {QStringLiteral("codex/event/token_count"), {}},
        {QStringLiteral("turn/started"), {QStringLiteral("turn.id")}},
        {QStringLiteral("item/agentMessage/delta"),
         {QStringLiteral("itemId"), QStringLiteral("delta")}},
        {QStringLiteral("item/commandExecution/outputDelta"),
         {QStringLiteral("itemId"), QStringLiteral("delta")}},
        {QStringLiteral("item/started"), {QStringLiteral("item.id"), QStringLiteral("item.type")}},
        {QStringLiteral("item/completed"),
         {QStringLiteral("item.id"), QStringLiteral("item.type")}},
        {QStringLiteral("turn/completed"), {}},
        {QStringLiteral("error"), {}},
    };
    return fields;
}

bool hasString(const QJsonObject& params, const QString& path) {
    const QStringList parts = path.split(QLatin1Char('.'));
    QJsonObject object = params;
    for (qsizetype index = 0; index + 1 < parts.size(); ++index) {
        const QJsonValue next = object.value(parts.at(index));
        if (!next.isObject())
            return false;
        object = next.toObject();
    }
    const QJsonValue value = object.value(parts.last());
    return value.isString() &&
           (parts.last() == QStringLiteral("delta") || !value.toString().isEmpty());
}

} // namespace

bool SessionHost::acceptProviderNotification(Session* session, const QString& method,
                                             const QJsonObject& params) {
    const auto handled = handledNotifications().constFind(method);
    if (handled == handledNotifications().cend()) {
        const QString key = ignoredNotifications_.contains(method) ||
                                    ignoredNotifications_.size() < maxIgnoredMethods
                                ? method
                                : QStringLiteral("(other)");
        ++ignoredNotifications_[key];
        return false;
    }
    QStringList missing;
    for (const QString& field : *handled) {
        if (!hasString(params, field))
            missing.append(field);
    }
    if (missing.isEmpty())
        return true;
    if (session == nullptr)
        return false;
    const QString key = method + QLatin1Char(' ') + missing.join(QLatin1Char(','));
    if (session->reportedProtocolErrors.contains(key))
        return false;
    session->reportedProtocolErrors.insert(key);
    qWarning("Codex notification %s is missing %s; skipped", qUtf8Printable(method),
             qUtf8Printable(missing.join(QStringLiteral(", "))));
    appendEvent(*session, QStringLiteral("provider.protocol_error"),
                {{QStringLiteral("provider"), QStringLiteral("codex")},
                 {QStringLiteral("method"), method},
                 {QStringLiteral("missing"), QJsonArray::fromStringList(missing)}});
    return false;
}

} // namespace ketplus::daemon
