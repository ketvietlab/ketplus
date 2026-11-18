#include "runtime/ManagedTaskSpec.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <limits>

namespace ketplus::daemon {
namespace {
bool keysAllowed(const QJsonObject& object, const QSet<QString>& keys) {
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!keys.contains(it.key()))
            return false;
    return true;
}
bool text(const QJsonValue& value, int maximum, bool allowEmpty = false) {
    return value.isString() && (allowEmpty || !value.toString().isEmpty()) &&
           !value.toString().contains(QChar::Null) && value.toString().toUtf8().size() <= maximum;
}
} // namespace

ManagedTaskAdmission validateManagedTask(const QJsonObject& payload,
                                         const ManagedTaskContext& context) {
    ManagedTaskAdmission result;
    const auto fail = [&](const QString& code, const QString& error) {
        result.errorCode = code;
        result.error = error;
        return result;
    };
    if (!keysAllowed(payload,
                     {"client_request_id", "kind", "title", "execution_spec", "timeout_ms"}) ||
        !text(payload.value("client_request_id"), 128) || !text(payload.value("title"), 256) ||
        !payload.value("execution_spec").isObject() || context.owner.sessionId.isEmpty() ||
        context.owner.generation <= 0)
        return fail("invalid_request", "A typed task spec, title and request key are required.");
    const auto kind = payload.value("kind").toString();
    if (kind != "process" && kind != "agent")
        return fail("invalid_request", "The task kind must be process or agent.");
    const auto timeoutValue = payload.value("timeout_ms");
    const auto timeout = timeoutValue.toDouble(-1);
    // JSON's exact integer range, leaving room for an epoch-millisecond deadline.
    if (!timeoutValue.isDouble() || !std::isfinite(timeout) || timeout <= 0 ||
        std::floor(timeout) != timeout || timeout > 9007199254740991.0)
        return fail("invalid_request",
                    "The timeout must be a positive, exact integer number of milliseconds.");
    auto execution = payload.value("execution_spec").toObject();
    const auto requestedCwd =
        execution.contains("cwd") ? execution.value("cwd") : QJsonValue(context.cwd);
    if (!text(requestedCwd, 4096))
        return fail("invalid_execution", "An existing working directory is required.");
    const auto cwd =
        QFileInfo(QDir(context.cwd).absoluteFilePath(requestedCwd.toString())).canonicalFilePath();
    if (cwd.isEmpty() || !QFileInfo(cwd).isDir() || !context.authorizeCwd ||
        !context.authorizeCwd(cwd))
        return fail("cwd_forbidden",
                    "The task directory is outside the owner's authorized workspace.");
    execution.insert("cwd", cwd);
    QJsonObject metadata{{"cwd", cwd}};
    if (kind == "process") {
        if (!keysAllowed(execution, {"argv", "explicit_shell_command", "cwd", "env_delta"}) ||
            execution.contains("argv") == execution.contains("explicit_shell_command") ||
            !execution.contains("env_delta") || !execution.value("env_delta").isObject() ||
            !payload.value("execution_spec").toObject().contains("cwd"))
            return fail("invalid_execution",
                        "A process needs argv or an explicit shell command, cwd and env_delta.");
        if (execution.contains("argv")) {
            const auto argv = execution.value("argv").toArray();
            if (!execution.value("argv").isArray() || argv.isEmpty() || argv.size() > 256)
                return fail("invalid_execution", "Process argv must contain 1 to 256 strings.");
            for (qsizetype i = 0; i < argv.size(); ++i)
                if (!text(argv[i], 64 * 1024, i != 0))
                    return fail("invalid_execution",
                                "Process arguments must be bounded strings without NUL bytes.");
        } else {
            if (!text(execution.value("explicit_shell_command"), 64 * 1024))
                return fail("invalid_execution",
                            "An explicit shell command must be a nonempty bounded string.");
            execution.insert("shell_command", execution.take("explicit_shell_command"));
        }
        static const QRegularExpression environmentKey("^[A-Za-z_][A-Za-z0-9_]{0,255}$");
        const auto delta = execution.value("env_delta").toObject();
        if (delta.size() > 128)
            return fail("invalid_execution", "The environment delta is too large.");
        for (auto it = delta.begin(); it != delta.end(); ++it)
            if (!environmentKey.match(it.key()).hasMatch() ||
                (!it.value().isNull() && !text(it.value(), 64 * 1024, true)))
                return fail("invalid_execution",
                            "The environment delta has an invalid key or value.");
        // Credential values stay in memory only. Persist an immutable fingerprint,
        // not a raw argv/shell/env copy that could expose tokens in the journal/UI.
        metadata.insert(
            "execution_digest",
            QString::fromLatin1(
                QCryptographicHash::hash(QJsonDocument(execution).toJson(QJsonDocument::Compact),
                                         QCryptographicHash::Sha256)
                    .toHex()));
    } else {
        if (!keysAllowed(execution, {"provider", "model", "effort", "prompt", "cwd"}) ||
            !text(execution.value("prompt"), 64 * 1024) ||
            execution.value("prompt").toString().trimmed().isEmpty())
            return fail(
                "invalid_execution",
                "A managed agent needs a bounded prompt and supported configuration fields.");
        for (const auto& key :
             {QStringLiteral("provider"), QStringLiteral("model"), QStringLiteral("effort")}) {
            if (execution.contains(key) && !text(execution.value(key), 256, true))
                return fail("invalid_execution", "Agent configuration values must be strings.");
        }
        const auto provider = execution.value("provider").toString(context.provider);
        const auto model = execution.value("model").toString(context.model);
        const auto effort = execution.value("effort").toString(context.effort);
        if (!context.authorizeAgent || !context.authorizeAgent(provider, model, effort))
            return fail("invalid_configuration",
                        "The agent provider, model or effort is unavailable.");
        execution.insert("provider", provider);
        execution.insert("model", model);
        execution.insert("effort", effort);
        metadata = execution; // Resolved child configuration must survive owner default changes.
    }
    if (QJsonDocument(execution).toJson(QJsonDocument::Compact).size() > 96 * 1024)
        return fail("invalid_execution", "The execution specification is too large.");
    const QJsonObject defaults{
        {"cwd", context.cwd},
        {"requested_cwd",
         payload.value("execution_spec").toObject().value("cwd").toString(context.cwd)},
        {"resolved_cwd", cwd},
        {"provider", context.provider},
        {"model", context.model},
        {"effort", context.effort}};
    metadata.insert("request_defaults", defaults);
    metadata.insert("origin_execution_policy", context.executionPolicy);
    const auto fingerprint = backgroundTaskFingerprint(payload, defaults);
    result.intent = {context.owner, payload.value("client_request_id").toString(),
                     kind,          payload.value("title").toString(),
                     fingerprint,   qint64(timeout),
                     metadata,      payload};
    result.execution = execution;
    return result;
}
} // namespace ketplus::daemon
