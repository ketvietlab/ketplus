#include "runtime/SessionHost.h"

#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"
#include "providers/compat/ProviderCapabilities.h"
#include "providers/compat/ProviderCompatibility.h"

#include <QJsonArray>
#include <QPointer>

#include <memory>

namespace ketplus::daemon {

void SessionHost::providerRuntimeStatus(
    std::function<void(const QJsonObject&, const QString&)> callback) {
    // Codex and Cursor answer asynchronously (a cached probe answers at once).
    struct Pending final {
        QJsonObject codex;
        QJsonObject cursor;
        int remaining{2};
    };
    auto pending = std::make_shared<Pending>();
    const QPointer<SessionHost> self(this);
    const auto finish = [self, pending, callback = std::move(callback)] {
        if (--pending->remaining > 0 || self.isNull())
            return;
        const ProviderAdapter* const claudeProvider = self->provider(QStringLiteral("claude"));
        const compat::Runtime claude =
            compat::bundledClaude(claudeProvider ? claudeProvider->executable() : QString());
        callback({{QStringLiteral("ok"), true},
                  {QStringLiteral("providers"),
                   QJsonArray{pending->codex, claude.toJson(), pending->cursor}}},
                 {});
    };
    adapter_->runtimeStatus([self, pending, finish](const compat::Runtime& runtime) {
        pending->codex = runtime.toJson();
        if (!self.isNull()) {
            // ADR 0015 §5: Codex notifications KetPlus did not act on, per method.
            QJsonObject ignored;
            for (auto it = self->ignoredNotifications_.cbegin();
                 it != self->ignoredNotifications_.cend(); ++it)
                ignored.insert(it.key(), it.value());
            pending->codex.insert(QStringLiteral("ignored_notifications"), ignored);
        }
        finish();
    });
    ProviderAdapter* const cursor = provider(QStringLiteral("cursor"));
    if (cursor == nullptr) {
        pending->cursor = compat::missing(compat::policy(QStringLiteral("cursor"))).toJson();
        finish();
        return;
    }
    cursor->runtimeStatus([pending, finish](const compat::Runtime& runtime) {
        pending->cursor = runtime.toJson();
        finish();
    });
}

ProviderAdapter* SessionHost::provider(const QString& adapter) const {
    for (ProviderAdapter* candidate : providers_)
        if (candidate->name() == adapter)
            return candidate;
    return nullptr;
}

ProviderAdapter* SessionHost::availableProvider(const QString& adapter) const {
    ProviderAdapter* const candidate = provider(adapter);
    return candidate != nullptr && candidate->isAvailable() ? candidate : nullptr;
}

QString SessionHost::missingProviderMessage(const QString& adapter) const {
    if (const ProviderAdapter* const candidate = provider(adapter))
        return candidate->missingMessage();
    if (adapter == QStringLiteral("claude"))
        return ClaudeAdapter::missingSidecarMessage();
    if (adapter == QStringLiteral("cursor"))
        return CursorAdapter::missingExecutableMessage();
    return QStringLiteral("The selected agent provider is not available.");
}

bool SessionHost::providerSupports(const QString& adapter, const QString& capability) const {
    // Claude answers from its policy baseline even before the sidecar is located.
    if (adapter == QStringLiteral("claude"))
        return compat::policy(QStringLiteral("claude")).baselineSupports(capability);
    const ProviderAdapter* const target =
        provider(adapter.isEmpty() ? QStringLiteral("codex") : adapter);
    return target != nullptr && target->supports(capability);
}

void SessionHost::noteProviderRuntime(const QString& sessionId, const qint64 generation) {
    const auto it = sessions_.constFind(sessionId);
    if (it == sessions_.cend() || it->stored.generation != generation)
        return;
    const auto record = [this, sessionId, generation](const compat::Runtime& runtime) {
        auto current = sessions_.find(sessionId);
        if (current == sessions_.end() || current->stored.generation != generation)
            return;
        // Outdated CLIs fail the turn with runtime.error; supported ones need no row.
        if (runtime.status != compat::SupportStatus::Newer &&
            runtime.status != compat::SupportStatus::Unknown)
            return;
        const QString key =
            QStringLiteral("%1 %2 %3")
                .arg(runtime.provider, runtime.version, compat::statusName(runtime.status));
        if (current->noticedRuntime == key)
            return;
        current->noticedRuntime = key;
        appendEvent(*current, QStringLiteral("provider.runtime"), runtime.toJson());
    };
    // The bundled Claude sidecar reports no runtime notice.
    if (it->stored.adapter == QStringLiteral("claude"))
        return;
    if (ProviderAdapter* const target =
            provider(it->stored.adapter.isEmpty() ? QStringLiteral("codex") : it->stored.adapter))
        target->runtimeStatus(record);
}

} // namespace ketplus::daemon
