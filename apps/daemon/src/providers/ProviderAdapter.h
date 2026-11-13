#pragma once

#include "providers/compat/ProviderCompatibility.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <functional>

namespace ketplus::daemon {

// The surface SessionHost drives on every provider process: Codex, Claude, Cursor and Copilot.
// ACP providers and Claude translate their protocol into canonical journal events in the adapter.
// Codex forwards its app-server notifications (CodexAdapter::notificationReceived), which
// SessionHost projects; that and its browser-settings reload stay Codex-specific.
class ProviderAdapter : public QObject {
    Q_OBJECT

  public:
    using Callback = std::function<void(const QJsonObject&, const QString&)>;

    using QObject::QObject;

    // The adapter name stored on a session: "codex", "claude", "cursor" or "copilot".
    [[nodiscard]] virtual QString name() const = 0;
    [[nodiscard]] virtual bool isAvailable() const = 0;
    [[nodiscard]] virtual bool isReady() const = 0;
    [[nodiscard]] virtual QString executable() const = 0;
    // Shown when a session needs this provider and it is not installed.
    [[nodiscard]] virtual QString missingMessage() const = 0;
    [[nodiscard]] virtual bool supports(const QString& capability) const = 0;
    virtual void runtimeStatus(std::function<void(const compat::Runtime&)> callback) = 0;
    virtual void call(const QString& method, const QJsonObject& params, Callback callback) = 0;
    virtual void configurationCatalog(Callback callback) = 0;
    virtual void respond(const QJsonValue& requestId, const QJsonObject& result) = 0;
    virtual void respondError(const QJsonValue& requestId, const QString& code,
                              const QString& message) = 0;
    virtual void stop() = 0;

  signals:
    // Providers that emit normalized transcript events (Claude and ACP).
    void canonicalEvent(const QString& sessionId, const QString& type, const QJsonObject& payload);
    void requestReceived(const QJsonValue& requestId, const QString& method,
                         const QJsonObject& params);
    void stopped(const QString& reason);
};

} // namespace ketplus::daemon
