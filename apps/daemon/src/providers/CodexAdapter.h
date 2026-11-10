#pragma once

#include "providers/ProviderAdapter.h"

#include "providers/ProviderLineReader.h"
#include "providers/compat/ProviderCapabilities.h"
#include "providers/compat/ProviderCompatibility.h"

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>

#include <functional>

class QProcess;
class QTimer;

namespace ketplus::browser_tools {
class CodexLaunchReloader;
}

namespace ketplus::daemon {

namespace compat {
class ProviderVersionProbe;
}

class CodexAdapter final : public ProviderAdapter {
    Q_OBJECT
    friend class ProviderStderrFloodTest;

  public:
    explicit CodexAdapter(const QString& executable = {}, QObject* parent = nullptr);
    ~CodexAdapter() override;

    [[nodiscard]] QString name() const override { return QStringLiteral("codex"); }
    [[nodiscard]] QString missingMessage() const override;
    [[nodiscard]] bool isAvailable() const override;
    [[nodiscard]] bool isReady() const override;
    [[nodiscard]] QString executable() const override;

    void call(const QString& method, const QJsonObject& params, Callback callback) override;
    // Version and support status of the Codex CLI (ADR 0015). Launches check it first and an
    // outdated CLI is not started.
    void runtimeStatus(std::function<void(const compat::Runtime&)> callback) override;
    // Whether the running CLI has `capability` (ADR 0015 §3). True until the version is known,
    // so a first call is still attempted; a "method not found" answer then removes it.
    [[nodiscard]] bool supports(const QString& capability) const override;
    // Adapter-owned translation from provider-specific model/list fields into
    // the provider-neutral configuration catalog consumed by SessionHost.
    void configurationCatalog(Callback callback) override;
    void respond(const QJsonValue& requestId, const QJsonObject& result) override;
    void respondError(const QJsonValue& requestId, const QString& code,
                      const QString& message) override;
    void stop() override;

    // Crash-restart backoff: 1 s doubling to 60 s, reset after a stable run.
    static constexpr int initialRestartDelayMs = 1000;
    static constexpr int maximumRestartDelayMs = 60 * 1000;
    static constexpr int stableRunMs = 60 * 1000;
    [[nodiscard]] int restartDelayMs() const;
    [[nodiscard]] bool restartScheduled() const;
    // Idle: no active turn (from turn/start until turn/completed or a final error), no in-flight
    // or queued call. A browser settings change restarts an
    // idle provider (sessions re-attach through thread/resume) and waits for a busy one.
    [[nodiscard]] bool isIdle() const;
    [[nodiscard]] browser_tools::CodexLaunchReloader* browserToolsReloader() const noexcept {
        return reloader_;
    }
    // Browser tool arguments the current Codex process was launched with (empty when browser
    // tools did not resolve). Only an injected `ketplus_browser` server is auto-approved.
    [[nodiscard]] const QStringList& launchedBrowserArguments() const noexcept {
        return launchedBrowserArguments_;
    }
    // Tests: replaces the user-settings lookup that yields the browser tool launch arguments.
    void setBrowserArgumentsProvider(std::function<QStringList()> provider);

  signals:
    void notificationReceived(const QString& method, const QJsonObject& params);
    // A notification too large to read was skipped; the process and its other threads go on.
    void notificationSkipped(const QString& method, const QString& threadId, qsizetype bytes);
    // A planned restart of an idle provider (reason "browser_settings"); not a failure.
    void reloaded(const QString& reason);

  private:
    struct QueuedCall final {
        QString method;
        QJsonObject params;
        Callback callback;
    };

    void ensureStarted();
    void initialize();
    void sendCall(const QString& method, const QJsonObject& params, Callback callback);
    void sendObject(const QJsonObject& object);
    void consumeStdout();
    void consumeLine(const QByteArray& line);
    void skipOversized(const OversizedProviderLine& line);
    void handleNotification(const QString& method, const QJsonObject& params);
    void failPending(const QString& reason);
    void setReady(bool ready);
    void noteUnexpectedStop();
    void trackTurn(const QString& method, const QJsonObject& params);
    void restartForBrowserSettings();
    [[nodiscard]] QStringList browserArguments() const;
    [[nodiscard]] static QString findExecutable();

    QString executable_;
    QProcess* process_{nullptr};
    ProviderLineReader stdoutReader_;
    QByteArray stderrBuffer_;
    QHash<quint64, Callback> pending_;
    QHash<quint64, QString> pendingMethods_;
    compat::CapabilityNarrowing narrowing_;
    QList<QueuedCall> queued_;
    quint64 nextRequestId_{1};
    bool starting_{false};
    bool ready_{false};
    bool stopping_{false};
    bool malformedOutput_{false};
    QTimer* restartTimer_{nullptr};
    QDeadlineTimer restartNotBefore_;
    QElapsedTimer readySince_;
    int restartDelayMs_{0};
    QSet<QString> activeTurns_;
    browser_tools::CodexLaunchReloader* reloader_{nullptr};
    std::function<QStringList()> browserArgumentsProvider_;
    QStringList launchedBrowserArguments_;
    compat::ProviderVersionProbe* versionProbe_{nullptr};
    bool checkingVersion_{false};
    bool versionCleared_{false};
};

} // namespace ketplus::daemon
