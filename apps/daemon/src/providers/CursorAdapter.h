#pragma once

#include "providers/ProviderExecutable.h"
#include "providers/acp/AcpAdapter.h"

#include "providers/compat/ProviderCapabilities.h"
#include "providers/compat/ProviderCompatibility.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QSet>
#include <QStringList>

#include <functional>

class QProcess;
class QTimer;

namespace ketplus::daemon {

namespace compat {
class ProviderVersionProbe;
}

// Cursor CLI ACP client (`agent acp`). The binary is discovered on PATH and is
// never bundled. Auth stays with the CLI (`agent login` or CURSOR_API_KEY).
class CursorAdapter final : public AcpAdapter {
    Q_OBJECT
    friend class ProviderStderrFloodTest;

  public:
    explicit CursorAdapter(const QString& executable = {}, QObject* parent = nullptr);
    ~CursorAdapter() override;

    [[nodiscard]] QString name() const override { return QStringLiteral("cursor"); }
    [[nodiscard]] QString missingMessage() const override;
    [[nodiscard]] bool isAvailable() const override;
    [[nodiscard]] QString executable() const override;

    void call(const QString& method, const QJsonObject& params, Callback callback) override;
    // Version and support status of the Cursor CLI (ADR 0015). Launches check it first and an
    // outdated CLI is not started.
    void runtimeStatus(std::function<void(const compat::Runtime&)> callback) override;
    // Whether the running CLI has `capability` (ADR 0015 §3); see CodexAdapter::supports.
    [[nodiscard]] bool supports(const QString& capability) const override;
    void configurationCatalog(Callback callback) override;
    void respond(const QJsonValue& requestId, const QJsonObject& result) override;
    void respondError(const QJsonValue& requestId, const QString& code,
                      const QString& message) override;

    [[nodiscard]] static QJsonArray modelCatalog();
    [[nodiscard]] static QJsonArray parseModelList(const QString& output);
    [[nodiscard]] static QString cliModelId(const QJsonArray& catalog, const QString& model,
                                            const QString& effort);
    // ACP session/new uses `default` for Auto; `auto` and `auto-smart` are invalid there.
    [[nodiscard]] static QString acpModelId(const QString& model);
    // Prefer advertised select values such as `composer-2.5[fast=false]`.
    [[nodiscard]] static QString acpModelConfigValue(const QString& model,
                                                     const QJsonArray& configOptions);
    [[nodiscard]] static QJsonArray permissionProfiles();
    [[nodiscard]] static QString permissionMode(const QString& profile);
    [[nodiscard]] static QString installCommand();
    [[nodiscard]] static QString loginCommand();
    [[nodiscard]] static QString missingExecutableMessage();
    [[nodiscard]] static QString notAuthenticatedMessage();
    // Surfaced when ACP refuses a named model (plan / Free / wrong CLI login).
    [[nodiscard]] static QString planLimitMessage();
    [[nodiscard]] static bool looksLikePlanLimit(const QString& message);
    // Cursor's account or team has used up its usage ("Your team has reached its usage limit").
    // Every model fails until it resets, so no login or model choice helps.
    [[nodiscard]] static bool looksLikeUsageLimit(const QString& message);
    // The reset Cursor names ("return on 11/15/2026"), as local midnight of that day in seconds
    // since the epoch; 0 when it names none.
    [[nodiscard]] static qint64 usageLimitResetsAt(const QString& message);
    // What a failed turn shows: Cursor's own words, explained when they need it.
    [[nodiscard]] static QString failureMessage(const QString& cursorMessage);
    // Email from ~/.cursor/cli-config.json (Agent CLI), not Cursor desktop UI auth.
    [[nodiscard]] static QString cliAuthEmail();
    // PATH plus the well-known install locations probed when PATH is stale
    // (GUI apps on Windows often miss %LOCALAPPDATA%\cursor-agent).
    [[nodiscard]] static QStringList wellKnownExecutableCandidates();
    struct ProcessInvocation final {
        QString program;
        QStringList arguments;
    };
    // .cmd/.bat go through cmd.exe; .ps1 through powershell. Unix is the file plus `acp`.
    // A non-empty model becomes `agent --model <id> acp` (Cursor's supported ACP pin).
    [[nodiscard]] static ProcessInvocation processInvocation(const QString& executable,
                                                             const QString& model = {});
    [[nodiscard]] static ProcessInvocation listModelsInvocation(const QString& executable);
    [[nodiscard]] static ProcessInvocation versionInvocation(const QString& executable);

  private:
    void configureProcess() override;
    QJsonObject clientCapabilities() const override;
    bool autoApprovePermission(const QString& sessionId,
                               const QJsonObject& toolCall) const override;
    void checkLaunch(std::function<void(const QString&)> done) override;
    void stopAuxiliaryProcesses() override;
    void resetProviderState() override;
    bool handleExtensionRequest(const QJsonObject& object) override;
    QString sessionPermissionMode(const QString& profile) const override;
    QString sessionError(const QString& error) const override;
    QString turnFailureMessage(const QString& error) const override;
    bool isFailureText(const QString& text) const override;
    bool holdAssistantText(const QString& text) const override;
    qint64 failureResetsAt(const QString& error) const override;
    void applySessionModel(const QString& acpSessionId, const QString& model,
                           const QJsonObject& sessionResult, std::function<void()> done) override;
    void listCliModels(Callback callback);
    void startCatalogProcess();
    void finishCatalog(const QJsonArray& models);
    // Ends an idle process that was started with `--model`, so the next turn starts one without.
    void restartUnpinned();
    [[nodiscard]] static QString findExecutable();
    [[nodiscard]] static bool looksLikeAuthError(const QString& message);
    ProviderExecutable executable_;
    QString pendingSpawnModel_;
    QString spawnModel_;
    QProcess* catalogProcess_{nullptr};
    QTimer* catalogTimer_{nullptr};
    QList<Callback> catalogWaiters_;
    QJsonArray cachedCatalog_;
    QElapsedTimer catalogFetchedAt_;
    QHash<QString, QJsonArray> questionOptionsByRequest_;
    QSet<QString> planRequests_;
    compat::ProviderVersionProbe* versionProbe_{nullptr};
};

} // namespace ketplus::daemon
