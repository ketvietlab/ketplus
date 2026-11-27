#include "runtime/SessionHost.h"

#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"
#include "providers/compat/ProviderCapabilities.h"
#include "providers/compat/ProviderCompatibility.h"
#include "runtime/ProviderErrorCode.h"

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
        QJsonObject copilot;
        int remaining{2};
    };
    auto pending = std::make_shared<Pending>();
    ProviderAdapter* const copilot = provider(QStringLiteral("copilot"));
    if (copilot != nullptr)
        ++pending->remaining;
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
        QJsonArray providers{pending->codex, claudeJson, pending->cursor};
        if (!pending->copilot.isEmpty())
            providers.append(pending->copilot);
        callback({{QStringLiteral("ok"), true}, {QStringLiteral("providers"), providers}}, {});
    };
    if (copilot != nullptr)
        copilot->runtimeStatus([self, pending, finish](const compat::Runtime& runtime) {
            pending->copilot = runtime.toJson();
            if (!self.isNull())
                pending->copilot.insert(
                    QStringLiteral("readiness"),
                    self->readiness_.compose(runtime, {}, QDateTime::currentSecsSinceEpoch()));
            finish();
        });
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

QJsonObject SessionHost::noteProviderOutcome(const Session& session, const QString& type,
                                             const QJsonObject& payload) {
    const QString adapter =
        session.stored.adapter.isEmpty() ? QStringLiteral("codex") : session.stored.adapter;
    if (type == QStringLiteral("turn.completed")) {
        if (payload.value(QStringLiteral("status")).toString() == QStringLiteral("completed"))
            readiness_.observe(adapter, QStringLiteral("ready"));
        return payload;
    }
    if (type != QStringLiteral("runtime.error"))
        return payload;
    QJsonObject classified = payload;
    const QString message = payload.value(QStringLiteral("message")).toString();
    // Only the daemon sets `code`; anything a provider put there is reclassified.
    const QString code =
        providerErrorCode(adapter, payload.value(QStringLiteral("provider_info")), message);
    if (code.isEmpty()) {
        classified.remove(QStringLiteral("code"));
        return classified;
    }
    classified.insert(QStringLiteral("code"), code);
    // The guide names the provider and model of the turn that failed, not the ones the
    // conversation has switched to since.
    classified.insert(QStringLiteral("provider"), adapter);
    if (!session.stored.model.isEmpty())
        classified.insert(QStringLiteral("model"), session.stored.model);
    const qint64 resetsAt = payload.value(QStringLiteral("resets_at")).toInteger();
    const QString state = readinessForErrorCode(code);
    if (!state.isEmpty())
        readiness_.observe(adapter, state, message, resetsAt);
    if (code == QStringLiteral("limit_reached") && resetsAt == 0 &&
        adapter == QStringLiteral("codex"))
        readCodexLimitReset(message);
    return classified;
}

void SessionHost::readCodexLimitReset(const QString& message) {
    const QPointer<SessionHost> self(this);
    adapter_->call(
        QStringLiteral("account/rateLimits/read"), {},
        [self, message](const QJsonObject& result, const QString& error) {
            // A turn that worked, or Check again, may have ended the limit meanwhile.
            if (self.isNull() || !error.isEmpty() ||
                self->readiness_.reportedState(QStringLiteral("codex")) !=
                    QStringLiteral("limit_reached"))
                return;
            // The window that is used up resets last; its reset ends the limit.
            qint64 resetsAt = 0;
            const QJsonObject limits = result.value(QStringLiteral("rateLimits")).toObject();
            for (const char* window : {"primary", "secondary"}) {
                const QJsonObject value = limits.value(QLatin1String(window)).toObject();
                if (value.value(QStringLiteral("usedPercent")).toDouble() >= 100)
                    resetsAt = qMax(resetsAt, value.value(QStringLiteral("resetsAt")).toInteger());
            }
            if (resetsAt > 0)
                self->readiness_.observe(QStringLiteral("codex"), QStringLiteral("limit_reached"),
                                         message, resetsAt);
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
