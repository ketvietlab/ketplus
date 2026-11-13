#pragma once

#include "providers/CopilotCli.h"
#include "providers/acp/AcpAdapter.h"
#include <QElapsedTimer>

namespace ketplus::daemon {
// User-installed GitHub Copilot CLI. Authentication stays with the CLI/environment.
class CopilotAdapter final : public AcpAdapter {
    Q_OBJECT
  public:
    explicit CopilotAdapter(const QString& executable = {}, QObject* parent = nullptr);
    ~CopilotAdapter() override;
    QString name() const override { return QStringLiteral("copilot"); }
    QString executable() const override;
    QString missingMessage() const override;
    bool isAvailable() const override;
    bool supports(const QString& capability) const override;
    void runtimeStatus(std::function<void(const compat::Runtime&)> done) override;
    void configurationCatalog(Callback done) override;
    void call(const QString& method, const QJsonObject& params, Callback done) override;

  protected:
    void configureProcess() override;
    void checkLaunch(std::function<void(const QString&)> done) override;
    QString initializationError(const QJsonObject& result) const override;
    QString responseError(const QJsonObject& error) override;
    QString processFailure(const QString& fallback, std::optional<int> exitCode) override;
    QString sessionError(const QString& error) const override;
    QString turnFailureMessage(const QString& error) const override;
    int requestTimeoutMs(const QString& method) const override;

  private:
    void refreshDiscovery() const;
    void authenticationFailed();
    const QString explicitPath_;
    mutable copilot_cli::DiscoveryResult discovery_;
    mutable QElapsedTimer lastDiscovery_;
    bool authenticationFailed_{false};
};
} // namespace ketplus::daemon
