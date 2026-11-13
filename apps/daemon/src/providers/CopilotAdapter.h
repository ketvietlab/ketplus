#pragma once

#include "providers/CopilotCatalog.h"
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
    bool strictPermissionRequests() const override { return true; }
    void checkLaunch(std::function<void(const QString&)> done) override;
    QString initializationError(const QJsonObject& result) const override;
    QString responseError(const QJsonObject& error) override;
    QString processFailure(const QString& fallback, std::optional<int> exitCode) override;
    QString sessionError(const QString& error) const override;
    QString turnFailureMessage(const QString& error) const override;
    int requestTimeoutMs(const QString& method) const override;
    void prepareSessionModel(const QString& sessionId, const QString& model,
                             const QJsonObject& result, Callback done) override;
    void handleSessionUpdate(const QJsonObject& params) override;
    void resetProviderState() override;
    void stopAuxiliaryProcesses() override;

  private:
    void refreshDiscovery() const;
    void authenticationFailed();
    const QString explicitPath_;
    mutable copilot_cli::DiscoveryResult discovery_;
    mutable QElapsedTimer lastDiscovery_;
    bool authenticationFailed_{false};
    CopilotCatalog* catalog_;
    QHash<QString, copilot_catalog::Snapshot> sessionModels_;
};
} // namespace ketplus::daemon
