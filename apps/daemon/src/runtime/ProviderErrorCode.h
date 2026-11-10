#pragma once

#include <QJsonValue>
#include <QString>

namespace ketplus::daemon {

// Why a provider ended a turn, as `runtime.error.code` (#170, ADR 0110): `auth_required`,
// `auth_expired`, `key_rejected`, `limit_reached`, `model_capacity`, `plan_blocked`,
// `provider_unavailable`, or empty when the error is about the work itself. Structured provider
// fields decide first (Codex `codexErrorInfo`); the provider's text is only the fallback, and
// the message itself is never changed.
[[nodiscard]] QString providerErrorCode(const QString& provider, const QJsonValue& providerInfo,
                                        const QString& message);
// The readiness state a code leaves the provider in; empty when the next turn may work.
[[nodiscard]] QString readinessForErrorCode(const QString& code);

} // namespace ketplus::daemon
