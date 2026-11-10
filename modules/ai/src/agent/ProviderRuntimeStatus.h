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

    // {"gated": bool, "providers": {id: [capability, ...]}, "setups": {id: guide}}. Direct
    // (test) mode is not gated. A provider that cannot start a turn has its setup guide
    // (ADR 0110, provider_setup::guide); one that is ready has none.
    [[nodiscard]] QVariantMap state() const { return state_; }
    [[nodiscard]] bool supports(const QString& provider, const QString& capability) const;
    [[nodiscard]] bool hasSetup(const QString& provider) const;
    // The guide for a turn the provider ended with a coded runtime.error, with the commands
    // the daemon last reported for the provider.
    [[nodiscard]] QVariantMap turnGuide(const QString& provider, const QString& code,
                                        const QString& message, const QString& model) const;
    // `recheck` asks the daemon to forget reported failures and probe again (Check again).
    void refresh(bool recheck = false);
    // Exposed for tests: builds the published state from a runtime_status providers array.
    [[nodiscard]] static QVariantMap stateFrom(const QJsonArray& providers);

  signals:
    void changed(const QVariantMap& state);

  private:
    void publish(const QVariantMap& state);

    CodexAppServer* server_;
    QVariantMap state_;
    QJsonArray providers_;
    quint64 generation_{0};
};

} // namespace ketplus
