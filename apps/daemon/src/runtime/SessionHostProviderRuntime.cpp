#include "runtime/SessionHost.h"

#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"
#include "providers/compat/ProviderCapabilities.h"
#include "providers/compat/ProviderCompatibility.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>

#include <memory>
#include <utility>

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
        QJsonObject claudeJson = claude.toJson();
        // Claude's sign-in is what its last query reported (#170); KetPlus never reads Claude
        // Code's credentials.
        claudeJson.insert(QStringLiteral("readiness"),
                          self->readiness_.compose(claude, {}, QDateTime::currentSecsSinceEpoch()));
        callback({{QStringLiteral("ok"), true},
                  {QStringLiteral("providers"),
                   QJsonArray{pending->codex, claudeJson, pending->cursor}}},
                 {});
    };
    adapter_->runtimeStatus([self, pending, finish](const compat::Runtime& runtime) {
        pending->codex = runtime.toJson();
        if (self.isNull()) {
            finish();
            return;
        }
        // ADR 0015 §5: Codex notifications KetPlus did not act on, per method.
        QJsonObject ignored;
        for (auto it = self->ignoredNotifications_.cbegin();
             it != self->ignoredNotifications_.cend(); ++it)
            ignored.insert(it.key(), it.value());
        pending->codex.insert(QStringLiteral("ignored_notifications"), ignored);
        self->codexReadiness(runtime, [self, pending, finish](const QJsonObject& readiness) {
            pending->codex.insert(QStringLiteral("readiness"), readiness);
            finish();
        });
    });
    ProviderAdapter* const cursor = provider(QStringLiteral("cursor"));
    const auto cursorDone = [self, pending, finish](const compat::Runtime& runtime) {
        pending->cursor = runtime.toJson();
        if (!self.isNull()) {
            const ProviderReadiness::Probe probe =
                runtime.status == compat::SupportStatus::Missing
                    ? ProviderReadiness::Probe{}
                    : ProviderReadiness::cursorProbe(
                          QDir::home().filePath(QStringLiteral(".cursor/cli-config.json")),
                          !qEnvironmentVariableIsEmpty("CURSOR_API_KEY"));
            pending->cursor.insert(
                QStringLiteral("readiness"),
                self->readiness_.compose(runtime, probe, QDateTime::currentSecsSinceEpoch()));
        }
        finish();
    };
    if (cursor == nullptr) {
        cursorDone(compat::missing(compat::policy(QStringLiteral("cursor"))));
        return;
    }
    cursor->runtimeStatus(cursorDone);
}

void SessionHost::codexReadiness(const compat::Runtime& runtime,
                                 std::function<void(const QJsonObject&)> callback) {
    const auto compose = [this, runtime](const ProviderReadiness::Probe& probe) {
        return readiness_.compose(runtime, probe, QDateTime::currentSecsSinceEpoch());
    };
    if (runtime.status == compat::SupportStatus::Missing ||
        runtime.status == compat::SupportStatus::Outdated) {
        callback(compose({}));
        return;
    }
    // account/read spends no quota. An app server that does not answer in time leaves the sign-in
    // unknown rather than holding up Settings.
    struct Once final {
        bool done{false};
    };
    auto once = std::make_shared<Once>();
    auto shared = std::make_shared<std::function<void(const QJsonObject&)>>(std::move(callback));
    const QPointer<SessionHost> self(this);
    QTimer::singleShot(codexAccountReadTimeoutMs, this, [once, shared, compose] {
        if (std::exchange(once->done, true))
            return;
        (*shared)(compose({}));
    });
    adapter_->call(QStringLiteral("account/read"), {{QStringLiteral("refreshToken"), false}},
                   [self, once, shared, compose](const QJsonObject& result, const QString& error) {
                       if (self.isNull() || std::exchange(once->done, true))
                           return;
                       (*shared)(compose(error.isEmpty() ? ProviderReadiness::codexProbe(result)
                                                         : ProviderReadiness::Probe{}));
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
