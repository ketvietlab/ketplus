#pragma once
#include "providers/ProviderAdapter.h"
#include "providers/compat/ProviderCapabilities.h"
#include <QDeadlineTimer>
#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QPair>
#include <QSet>
#include <optional>
class QProcess;
class QTimer;
namespace ketplus::daemon {
// Shared ACP stdio/session/transcript engine. Subclasses own CLI discovery, launch
// policy, catalogs and provider extensions; this class never authenticates a CLI.
class AcpAdapter : public ProviderAdapter {
    Q_OBJECT
    friend class ProviderStderrFloodTest;

  public:
    explicit AcpAdapter(QString label, QObject* parent = nullptr);
    ~AcpAdapter() override;
    bool isReady() const override { return ready_; }
    void call(const QString& method, const QJsonObject& params, Callback callback) override;
    void respond(const QJsonValue& requestId, const QJsonObject& result) override;
    void respondError(const QJsonValue& requestId, const QString& code,
                      const QString& message) override;
    void stop() override;

  protected:
    // Set process_ program/arguments; the engine owns channels, environment and signals.
    virtual void configureProcess() = 0;
    virtual QJsonObject clientCapabilities() const;
    // Opt in to turn-bound permission IDs and strictly advertised one-time options.
    virtual bool strictPermissionRequests() const { return false; }
    // rawInput is provider-owned data, not a portable authenticated MCP identity.
    virtual bool autoApprovePermission(const QString&, const QJsonObject&) const { return false; }
    virtual QString initializationError(const QJsonObject&) const { return {}; }
    virtual QString responseError(const QJsonObject& error);
    virtual QString processFailure(const QString& fallback, std::optional<int>) { return fallback; }
    // Zero keeps the existing provider policy. The engine owns timeout settlement/cleanup.
    virtual int requestTimeoutMs(const QString&) const { return 0; }
    // The callback reports an empty error when launching is allowed.
    virtual void checkLaunch(std::function<void(const QString&)> done) { done({}); }
    // Called while the concrete adapter is alive by its explicit stop()/destructor.
    virtual void stopAuxiliaryProcesses() {}
    virtual void resetProviderState() {}
    // Receives the full session/new or session/load result (including model catalogs).
    // Session binding waits for done(), even when a provider cannot select a model.
    virtual void applySessionModel(const QString&, const QString&, const QJsonObject&,
                                   std::function<void()> done) {
        done();
    }
    // Translate host policy into the provider's advertised ACP mode identifier.
    virtual QString sessionPermissionMode(const QString&) const { return {}; }
    virtual QString sessionError(const QString& error) const { return error; }
    virtual QString turnFailureMessage(const QString& error) const { return error; }
    virtual bool isFailureText(const QString&) const { return false; }
    virtual bool holdAssistantText(const QString&) const { return false; }
    virtual qint64 failureResetsAt(const QString&) const { return 0; }
    // Return true when a provider-specific server request was handled.
    virtual bool handleExtensionRequest(const QJsonObject&) { return false; }
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
    void initialize();
    void startTurn(const QJsonObject& params, Callback callback);
    void interruptTurn(const QJsonObject& params, Callback callback);
    void ensureAcpSession(const QString& ketplusSessionId, const QString& cwd,
                          const QString& resume, const QString& model, Callback callback);
    void applyConfigOptions(const QString& acpSessionId, QList<QPair<QString, QString>> options,
                            std::function<void()> done);
    void sendPrompt(TurnState& turn, const QJsonObject& params);
    void emitContextUsage(const QString& sessionId, const QJsonObject& source);
    void finishTurn(const QString& ketplusSessionId, const QString& status,
                    const QString& errorMessage = {});
    static QString requestKey(const QJsonValue& id);
    void sendCall(const QString& method, const QJsonObject& params, Callback callback);
    void sendObject(const QJsonObject& object);
    void consumeStdout();
    void consumeLine(const QByteArray& line);
    virtual void handleSessionUpdate(const QJsonObject& params);
    void handlePermissionRequest(const QJsonValue& requestId, const QJsonObject& params);
    void handleStrictPermission(const QJsonValue& requestId, const QJsonObject& params);
    void respondStrictPermission(const QJsonValue& requestId, const QJsonObject& result);
    void cancelPermissions(const QString& sessionId = {});
    void sendPermissionOutcome(const QJsonValue& requestId, const QString& optionId = {});
    struct PermissionRequest {
        QJsonValue providerId;
        QString sessionId;
        QString turnId;
        QJsonArray options;
    };
    QHash<QString, PermissionRequest> permissions_;
    quint64 nextPermissionNumber_{1};
    void failPending(const QString& reason);
    void setReady(bool ready);
    void noteUnexpectedStop();
    // Append the bounded stderr tail to process failures.
    [[nodiscard]] QString stderrTail() const;
    [[nodiscard]] QJsonArray promptContent(const QJsonObject& params) const;

    QString label_;
    QProcess* process_{nullptr};
    QByteArray stdoutBuffer_;
    QByteArray stderrBuffer_;
    QHash<quint64, Callback> pending_;
    QHash<quint64, QTimer*> pendingTimers_;
    QHash<quint64, QString> pendingMethods_;
    compat::CapabilityNarrowing narrowing_;
    QList<QueuedCall> queued_;
    QHash<QString, QString> acpSessionByKetplus_;
    QHash<QString, QString> ketplusByAcp_;
    QHash<QString, TurnState> turns_;
    QHash<QString, QJsonArray> permissionOptionsByRequest_;
    QHash<QString, QString> permissionBySession_;
    QSet<QString> startingSessions_;
    // Sessions given the KetPlus browser server, with the file that signals their turn ends.
    QHash<QString, QString> browserSessions_;
    // ADR 0027: each conversation's ketplus_agents server and routing as SessionHost supplied
    // them, and the sessions whose ACP session received that server.
    QHash<QString, QJsonObject> agentServers_;
    QHash<QString, QString> agentInstructions_;
    QSet<QString> agentSessions_;
    // Sessions given the KetPlus Computer server (ADR 0030).
    QSet<QString> computerSessions_;
    quint64 nextRequestId_{1};
    quint64 nextTurnNumber_{1};
    bool starting_{false};
    bool ready_{false};
    bool stopping_{false};
    bool malformedOutput_{false};
    QTimer* restartTimer_{nullptr};
    QDeadlineTimer restartNotBefore_;
    int restartDelayMs_{0};
    bool checkingLaunch_{false};
    bool launchCleared_{false};
};
} // namespace ketplus::daemon
