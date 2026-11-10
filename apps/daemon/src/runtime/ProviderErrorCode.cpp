#include "runtime/ProviderErrorCode.h"

#include "providers/CursorAdapter.h"

#include <QJsonObject>

namespace ketplus::daemon {
namespace {
// Codex serializes CodexErrorInfo as a string, or as an object keyed by the variant when it
// carries data (`{"httpConnectionFailed": {...}}`).
QString variant(const QJsonValue& info) {
    if (info.isString())
        return info.toString();
    if (info.isObject() && !info.toObject().isEmpty())
        return info.toObject().begin().key();
    return {};
}

bool containsAny(const QString& text, std::initializer_list<const char*> needles) {
    for (const char* needle : needles)
        if (text.contains(QLatin1String(needle)))
            return true;
    return false;
}
} // namespace

QString providerErrorCode(const QString& provider, const QJsonValue& providerInfo,
                          const QString& message) {
    const QString info = variant(providerInfo);
    if (info == QStringLiteral("usageLimitExceeded"))
        return QStringLiteral("limit_reached");
    if (info == QStringLiteral("unauthorized"))
        return QStringLiteral("auth_required");
    if (info == QStringLiteral("serverOverloaded"))
        return QStringLiteral("model_capacity");

    const QString text = message.toLower();
    if (containsAny(text, {"sidecar was not found", "cli was not found"}))
        return QStringLiteral("provider_unavailable");
    if (containsAny(text, {"rejected this api key", "invalid x-api-key", "invalid api key"}))
        return QStringLiteral("key_rejected");
    if (containsAny(text, {"oauth session expired", "could not be refreshed", "token has expired",
                           "token expired", "session expired"}))
        return QStringLiteral("auth_expired");
    if (provider == QStringLiteral("cursor") && CursorAdapter::looksLikePlanLimit(message))
        return QStringLiteral("plan_blocked");
    if (text.startsWith(QStringLiteral("cursor blocked this model")))
        return QStringLiteral("plan_blocked");
    // CursorAdapter already rewrites its auth errors as "Cursor CLI is not signed in".
    if (containsAny(text, {"not signed in", "not logged in", "authentication required",
                           "not authenticated", "please run /login", "login required"}))
        return QStringLiteral("auth_required");
    if (containsAny(text, {"hit your weekly limit", "hit your usage limit", "usage limit reached",
                           "you've hit your", "you have hit your", "limit reached · resets",
                           "usage limit"}))
        return QStringLiteral("limit_reached");
    if (containsAny(text, {"at capacity", "overloaded"}))
        return QStringLiteral("model_capacity");
    return {};
}

QString readinessForErrorCode(const QString& code) {
    if (code == QStringLiteral("auth_required"))
        return QStringLiteral("signed_out");
    if (code == QStringLiteral("auth_expired"))
        return QStringLiteral("expired");
    if (code == QStringLiteral("key_rejected") || code == QStringLiteral("limit_reached"))
        return code;
    // Capacity is about one model at one moment, a plan block about one model; neither stops
    // the provider. A missing runtime is the version check's to report.
    return {};
}

} // namespace ketplus::daemon
