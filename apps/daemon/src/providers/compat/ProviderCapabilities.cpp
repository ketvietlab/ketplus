#include "providers/compat/ProviderCapabilities.h"

namespace ketplus::daemon::compat {

QString capabilityForMethod(const QStringView method) {
    // Codex app-server methods and their ACP counterparts.
    if (method == u"turn/steer")
        return capability::turnSteer;
    if (method == u"turn/interrupt" || method == u"session/cancel")
        return capability::turnInterrupt;
    if (method == u"thread/resume" || method == u"session/load")
        return capability::threadResume;
    if (method == u"thread/name/set")
        return capability::threadRename;
    if (method == u"thread/fork")
        return capability::threadFork;
    if (method == u"thread/compact/start")
        return capability::threadCompact;
    if (method == u"account/rateLimits/read")
        return capability::rateLimits;
    if (method == u"model/list")
        return capability::modelList;
    return {};
}

void CapabilityNarrowing::remove(const QString& capability) {
    if (!capability.isEmpty())
        removed_.insert(capability);
}

QString CapabilityNarrowing::methodNotFound(const QStringView method) {
    const QString capability = capabilityForMethod(method);
    remove(capability);
    return capability;
}

void CapabilityNarrowing::acpInitialized(const QJsonObject& result) {
    const QJsonObject agent = result.value(QStringLiteral("agentCapabilities")).toObject();
    const auto explicitlyFalse = [](const QJsonObject& object, const QString& key) {
        const QJsonValue value = object.value(key);
        return value.isBool() && !value.toBool();
    };
    if (explicitlyFalse(agent, QStringLiteral("loadSession")))
        remove(capability::threadResume);
    if (explicitlyFalse(agent.value(QStringLiteral("promptCapabilities")).toObject(),
                        QStringLiteral("image")))
        remove(capability::images);
}

Runtime CapabilityNarrowing::apply(Runtime runtime) const {
    runtime.capabilities.removeIf(
        [this](const QString& capability) { return removed_.contains(capability); });
    return runtime;
}

} // namespace ketplus::daemon::compat
