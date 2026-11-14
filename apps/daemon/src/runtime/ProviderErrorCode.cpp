#include "runtime/ProviderErrorCode.h"

#include "providers/CursorAdapter.h"

#include <QJsonObject>
#include <QRegularExpression>

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

// A provider's own sentence, at the start of the message (after an "API Error: 401" prefix).
// Anchored so that an error about the work, such as "MCP server github: authentication
// required", never changes the provider's readiness.
bool opens(const QString& message, const char* pattern) {
    const QRegularExpression expression(
        QStringLiteral("^(?:api error: \\d{3}\\s*)?(?:%1)").arg(QString::fromUtf8(pattern)),
        QRegularExpression::CaseInsensitiveOption);
    return expression.match(message.trimmed()).hasMatch();
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

    if (opens(message, "[\\w .-]+ (?:sidecar|cli) was not found"))
        return QStringLiteral("provider_unavailable");
    if (opens(message, "selected model is at capacity|overloaded\\b|\\{.*overloaded_error"))
        return QStringLiteral("model_capacity");
    // Before the plan check: a used-up Cursor team fails every model until it resets.
    if (provider == QStringLiteral("cursor") && CursorAdapter::looksLikeUsageLimit(message))
        return QStringLiteral("limit_reached");
    if (provider == QStringLiteral("cursor") &&
        (CursorAdapter::looksLikePlanLimit(message) || opens(message, "cursor blocked this model")))
        return QStringLiteral("plan_blocked");
    if (provider == QStringLiteral("copilot") && opens(message, "github copilot cli needs sign-in"))
        return QStringLiteral("auth_required");
    // Codex named another kind of failure: its words are not about the account.
    if (!info.isEmpty())
        return {};
    if (opens(message, "anthropic rejected this api key|invalid x-api-key|"
                       "invalid api key(?! · please run /login)"))
        return QStringLiteral("key_rejected");
    if (opens(message, "(?:failed to authenticate: )?(?:oauth session expired|oauth token has "
                       "expired|your access token could not be refreshed|your refresh token "
                       "has expired)"))
        return QStringLiteral("auth_expired");
    // CursorAdapter already rewrites its auth errors as "Cursor CLI is not signed in".
    if (opens(message, "cursor cli is not signed in|authentication required|not logged in|"
                       "invalid api key · please run /login|please run /login"))
        return QStringLiteral("auth_required");
    if (opens(message, "you've hit your|you have hit your|claude ai usage limit reached|"
                       "you've reached your usage limit"))
        return QStringLiteral("limit_reached");
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
