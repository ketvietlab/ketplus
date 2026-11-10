#include "agent/ProviderRuntimeStatus.h"

#include "agent/CodexAppServer.h"
#include "agent/ProviderSetupGuide.h"

#include <QJsonObject>
#include <QPointer>

namespace ketplus {

ProviderRuntimeStatus::ProviderRuntimeStatus(CodexAppServer* server, QObject* parent)
    : QObject(parent), server_(server) {
    state_ = {{QStringLiteral("gated"), server_->usesDaemon()}};
    connect(server_, &CodexAppServer::readyChanged, this, [this](const bool ready) {
        if (ready)
            refresh();
    });
    // A daemon restart may bring a different provider CLI.
    connect(server_, &CodexAppServer::serverStopped, this, [this] {
        ++generation_;
        providers_ = {};
        publish({{QStringLiteral("gated"), server_->usesDaemon()}});
    });
}

QVariantMap ProviderRuntimeStatus::stateFrom(const QJsonArray& providers) {
    QVariantMap capabilities;
    QVariantMap setups;
    for (const QJsonValue& value : providers) {
        const QJsonObject provider = value.toObject();
        const QString id = provider.value(QStringLiteral("provider")).toString();
        if (id.isEmpty())
            continue;
        capabilities.insert(id, provider.value(QStringLiteral("capabilities")).toVariant());
        // A capacity or plan error is about one model and stays with its turn.
        const QVariantMap guide = provider_setup::guide(provider);
        if (!guide.isEmpty() && !guide.value(QStringLiteral("transient")).toBool())
            setups.insert(id, guide);
    }
    return {{QStringLiteral("gated"), true},
            {QStringLiteral("providers"), capabilities},
            {QStringLiteral("setups"), setups}};
}

bool ProviderRuntimeStatus::supports(const QString& provider, const QString& capability) const {
    if (!state_.value(QStringLiteral("gated")).toBool())
        return true;
    // Until the daemon answers, nothing optional is offered.
    return state_.value(QStringLiteral("providers"))
        .toMap()
        .value(provider)
        .toStringList()
        .contains(capability);
}

bool ProviderRuntimeStatus::hasSetup(const QString& provider) const {
    return state_.value(QStringLiteral("setups")).toMap().contains(provider);
}

QVariantMap ProviderRuntimeStatus::turnGuide(const QString& provider, const QString& code,
                                             const QString& message, const QString& model) const {
    QJsonObject entry{{QStringLiteral("provider"), provider}};
    for (const QJsonValue& value : providers_)
        if (value.toObject().value(QStringLiteral("provider")).toString() == provider)
            entry = value.toObject();
    return provider_setup::guide(
        entry, {{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}, model);
}

void ProviderRuntimeStatus::refresh(const bool recheck) {
    if (!server_->usesDaemon() || !server_->isAvailable())
        return;
    const quint64 generation = ++generation_;
    server_->call(QStringLiteral("provider/runtimeStatus"), {{QStringLiteral("recheck"), recheck}},
                  [self = QPointer<ProviderRuntimeStatus>(this),
                   generation](const QJsonObject& result, const QString& error) {
                      if (!self || generation != self->generation_ || !error.isEmpty())
                          return;
                      self->providers_ = result.value(QStringLiteral("providers")).toArray();
                      self->publish(stateFrom(self->providers_));
                  });
}

void ProviderRuntimeStatus::publish(const QVariantMap& state) {
    if (state == state_)
        return;
    state_ = state;
    emit changed(state_);
}

} // namespace ketplus
