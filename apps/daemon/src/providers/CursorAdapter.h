#pragma once

#include "providers/compat/ProviderCapabilities.h"
#include "providers/compat/ProviderCompatibility.h"

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPair>
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
class CursorAdapter final : public QObject {
    Q_OBJECT

  public:
    using Callback = std::function<void(const QJsonObject&, const QString&)>;

    explicit CursorAdapter(const QString& executable = {}, QObject* parent = nullptr);
    ~CursorAdapter() override;

    [[nodiscard]] bool isAvailable() const;
    [[nodiscard]] bool isReady() const;
    [[nodiscard]] QString executable() const;

    void call(const QString& method, const QJsonObject& params, Callback callback);
    // Version and support status of the Cursor CLI (ADR 0015). Launches check it first and an
    // outdated CLI is not started.
    void runtimeStatus(std::function<void(const compat::Runtime&)> callback);
    // Whether the running CLI has `capability` (ADR 0015 §3); see CodexAdapter::supports.
    [[nodiscard]] bool supports(const QString& capability) const;
    void configurationCatalog(Callback callback);
    void respond(const QJsonValue& requestId, const QJsonObject& result);
    void respondError(const QJsonValue& requestId, const QString& code, const QString& message);
    void stop();

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

  signals:
    void canonicalEvent(const QString& sessionId, const QString& type, const QJsonObject& payload);
    void requestReceived(const QJsonValue& requestId, const QString& method,
                         const QJsonObject& params);
    void stopped(const QString& reason);

  private:
    struct QueuedCall final {
        QString method;
        QJsonObject params;
        Callback callback;
    };

    struct TurnState final {
        QString ketplusSessionId;
        QString acpSessionId;
        QString turnId;
        QString itemId;
        QString assistantText;
        QString permission;
        bool heldAssistant{false};
        bool interrupted{false};
        QHash<QString, QJsonObject> toolItems;
    };

    void ensureStarted();
    void configureProcess();
    void initialize();
    void startTurn(const QJsonObject& params, Callback callback);
    void interruptTurn(const QJsonObject& params, Callback callback);
    void ensureAcpSession(const QString& ketplusSessionId, const QString& cwd,
                          const QString& resume, const QString& model, Callback callback);
    void applySessionModel(const QString& acpSessionId, const QString& model,
                           const QJsonArray& advertisedOptions, std::function<void()> done);
    void applyConfigOptions(const QString& acpSessionId, QList<QPair<QString, QString>> options,
                            std::function<void()> done);
    void listCliModels(Callback callback);
    void startCatalogProcess();
    void finishCatalog(const QJsonArray& models);
    void sendPrompt(TurnState& turn, const QJsonObject& params);
    void emitContextUsage(const QString& sessionId, const QJsonObject& source);
    void finishTurn(const QString& ketplusSessionId, const QString& status,
                    const QString& errorMessage = {});
    void sendCall(const QString& method, const QJsonObject& params, Callback callback);
    void sendObject(const QJsonObject& object);
    void consumeStdout();
    void consumeLine(const QByteArray& line);
    void handleSessionUpdate(const QJsonObject& params);
    void handlePermissionRequest(const QJsonValue& requestId, const QJsonObject& params);
    void failPending(const QString& reason);
    void setReady(bool ready);
    void noteUnexpectedStop();
    [[nodiscard]] static QString findExecutable();
    [[nodiscard]] static bool looksLikeAuthError(const QString& message);
    // The tail of the CLI's stderr, appended to failures so the user sees why it died.
    [[nodiscard]] QString stderrTail() const;
    [[nodiscard]] QJsonArray promptContent(const QJsonObject& params) const;

    QString executable_;
    QString pendingSpawnModel_;
    QString spawnModel_;
    QProcess* process_{nullptr};
    QProcess* catalogProcess_{nullptr};
    QTimer* catalogTimer_{nullptr};
    QList<Callback> catalogWaiters_;
    QJsonArray cachedCatalog_;
    QElapsedTimer catalogFetchedAt_;
    QByteArray stdoutBuffer_;
    QByteArray stderrBuffer_;
    QHash<quint64, Callback> pending_;
    QHash<quint64, QString> pendingMethods_;
    compat::CapabilityNarrowing narrowing_;
    QList<QueuedCall> queued_;
    QHash<QString, QString> acpSessionByKetplus_;
    QHash<QString, QString> ketplusByAcp_;
    QHash<QString, TurnState> turns_;
    QHash<QString, QJsonArray> questionOptionsByRequest_;
    QHash<QString, QJsonArray> permissionOptionsByRequest_;
    QSet<QString> planRequests_;
    QHash<QString, QString> permissionBySession_;
    QSet<QString> startingSessions_;
    // Sessions given the KetPlus browser server, with the file that signals their turn ends.
    QHash<QString, QString> browserSessions_;
    // ADR 0027: each conversation's ketplus_agents server and routing as SessionHost supplied
    // them, and the sessions whose ACP session received that server.
    QHash<QString, QJsonObject> agentServers_;
    QHash<QString, QString> agentInstructions_;
    QSet<QString> agentSessions_;
    quint64 nextRequestId_{1};
    quint64 nextTurnNumber_{1};
    bool starting_{false};
    bool ready_{false};
    bool stopping_{false};
    QTimer* restartTimer_{nullptr};
    QDeadlineTimer restartNotBefore_;
    int restartDelayMs_{0};
    compat::ProviderVersionProbe* versionProbe_{nullptr};
    bool checkingVersion_{false};
    bool versionCleared_{false};
};

} // namespace ketplus::daemon
