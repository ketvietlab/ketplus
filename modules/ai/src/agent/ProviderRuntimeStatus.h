#pragma once

#include <QJsonArray>
#include <QObject>
#include <QVariantMap>

namespace ketplus {
class CodexAppServer;

// Provider capabilities reported by the daemon (`provider.runtime_status`, ADR 0015). Desktop
// gates provider-specific actions such as /compact on these names, never on CLI versions.
class ProviderRuntimeStatus final : public QObject {
    Q_OBJECT
  public:
    explicit ProviderRuntimeStatus(CodexAppServer* server, QObject* parent = nullptr);

    // {"gated": bool, "providers": {id: [capability, ...]}}. Direct (test) mode is not gated.
    [[nodiscard]] QVariantMap state() const { return state_; }
    [[nodiscard]] bool supports(const QString& provider, const QString& capability) const;
    void refresh();
    // Exposed for tests: builds the published state from a runtime_status providers array.
    [[nodiscard]] static QVariantMap stateFrom(const QJsonArray& providers);

  signals:
    void changed(const QVariantMap& state);

  private:
    void publish(const QVariantMap& state);

    CodexAppServer* server_;
    QVariantMap state_;
    quint64 generation_{0};
};

} // namespace ketplus
