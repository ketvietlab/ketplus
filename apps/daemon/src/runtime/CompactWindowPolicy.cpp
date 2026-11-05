#include "runtime/CompactWindowPolicy.h"

namespace ketplus::daemon::context_harness {
namespace {
QJsonObject failure(const QString& code, const QString& message) {
    return {{"ok", false}, {"error_code", code}, {"error_message", message}};
}
} // namespace

QJsonObject resolveCompactWindow(const QJsonValue& value, const QString& provider,
                                 const qint64 inherited, const QJsonArray&, qint64& resolved) {
    resolved = inherited;
    if (value.isNull()) {
        resolved = 0;
    } else if (!value.isUndefined()) {
        resolved = value.toInteger(-1);
        if (!value.isDouble() || resolved < minimumCompactWindow || resolved > maximumCompactWindow)
            return failure(QStringLiteral("invalid_compact_window"),
                           QStringLiteral("compactWindow must be null or an integer between "
                                          "100000 and 1000000 tokens."));
    }
    if (resolved != 0 && provider != "codex" && provider != "claude")
        return failure(QStringLiteral("unsupported_compact_window"),
                       QStringLiteral("This provider does not support compactWindow."));
    return {};
}

QJsonValue compactWindowJson(const qint64 tokens) {
    return tokens > 0 ? QJsonValue(tokens) : QJsonValue(QJsonValue::Null);
}

QJsonObject codexCompactConfig(const qint64 tokens) {
    if (tokens == 0)
        return {};
    return {{QStringLiteral("model_auto_compact_token_limit"), tokens},
            {QStringLiteral("model_auto_compact_token_limit_scope"), QStringLiteral("total")}};
}

} // namespace ketplus::daemon::context_harness
