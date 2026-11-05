#include "agent/ProviderRuntimeStatus.h"

#include "agent/CodexAppServer.h"

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
        publish({{QStringLiteral("gated"), server_->usesDaemon()}});
    });
}

QVariantMap ProviderRuntimeStatus::stateFrom(const QJsonArray& providers) {
    QVariantMap capabilities;
    for (const QJsonValue& value : providers) {
        const QJsonObject provider = value.toObject();
        const QString id = provider.value(QStringLiteral("provider")).toString();
        if (!id.isEmpty())
            capabilities.insert(id, provider.value(QStringLiteral("capabilities")).toVariant());
    }
    return {{QStringLiteral("gated"), true}, {QStringLiteral("providers"), capabilities}};
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

void ProviderRuntimeStatus::refresh() {
    if (!server_->usesDaemon() || !server_->isAvailable())
        return;
    const quint64 generation = ++generation_;
    server_->call(QStringLiteral("provider/runtimeStatus"), {},
                  [self = QPointer<ProviderRuntimeStatus>(this),
                   generation](const QJsonObject& result, const QString& error) {
                      if (!self || generation != self->generation_ || !error.isEmpty())
                          return;
                      self->publish(stateFrom(result.value(QStringLiteral("providers")).toArray()));
                  });
}

void ProviderRuntimeStatus::publish(const QVariantMap& state) {
    if (state == state_)
        return;
    state_ = state;
    emit changed(state_);
}

} // namespace ketplus
