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
        const compat::Runtime claude = self->claude_ != nullptr
                                           ? compat::bundledClaude(self->claude_->executable())
                                           : compat::bundledClaude({});
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
    if (cursor_ == nullptr) {
        pending->cursor = compat::missing(compat::policy(QStringLiteral("cursor"))).toJson();
        finish();
        return;
    }
    cursor_->runtimeStatus([pending, finish](const compat::Runtime& runtime) {
        pending->cursor = runtime.toJson();
        finish();
    });
}

bool SessionHost::providerSupports(const QString& adapter, const QString& capability) const {
    if (adapter == QStringLiteral("cursor"))
        return cursor_ != nullptr && cursor_->supports(capability);
    if (adapter == QStringLiteral("claude"))
        return compat::policy(QStringLiteral("claude")).baselineSupports(capability);
    return adapter_->supports(capability);
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
    if (it->stored.adapter == QStringLiteral("cursor")) {
        if (cursor_ != nullptr)
            cursor_->runtimeStatus(record);
    } else if (it->stored.adapter != QStringLiteral("claude")) {
        adapter_->runtimeStatus(record);
    }
}

} // namespace ketplus::daemon
