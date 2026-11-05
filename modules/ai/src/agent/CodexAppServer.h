#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantList>

#include <functional>

class QProcess;

namespace ketplus::browser_tools {
class CodexLaunchReloader;
}

namespace ketplus {

namespace remote {
class DaemonClient;
}

class CodexAppServer final : public QObject {
    Q_OBJECT

  public:
    using ResponseCallback = std::function<void(const QJsonObject&, const QString&)>;

    explicit CodexAppServer(QObject* parent = nullptr);
    explicit CodexAppServer(const QString& executable, QObject* parent = nullptr);
    ~CodexAppServer() override;

    [[nodiscard]] bool isAvailable() const noexcept;
    [[nodiscard]] bool isReady() const noexcept;
    [[nodiscard]] QString executable() const;
    [[nodiscard]] bool usesDaemon() const noexcept { return daemon_ != nullptr; }
    [[nodiscard]] bool hasCompactWindowSupport() const noexcept;
    [[nodiscard]] bool isRestoring() const noexcept { return restoring_; }
    [[nodiscard]] bool hasDaemonTranscriptWindow() const noexcept {
        return daemonHasTranscriptWindow_;
    }
    // Forward (after_cursor) window paging and the daemon's turn_limit ceiling.
    [[nodiscard]] bool hasDaemonTranscriptAfterCursor() const noexcept {
        return daemonHasTranscriptWindow_ && daemonHasTranscriptAfterCursor_;
    }
    [[nodiscard]] int daemonTranscriptWindowMaxTurns() const noexcept {
        return daemonTranscriptWindowMaxTurns_;
    }
    [[nodiscard]] bool hasDaemonActivityGroups() const noexcept;
    void readDaemonActivityGroup(const QString& sessionId, const QJsonObject& params,
                                 ResponseCallback callback);
    // A catch-up head read can refresh the fence without releasing its live buffer.
    void readDaemonTranscriptWindow(const QString& sessionId, const QJsonObject& params,
                                    ResponseCallback callback, bool preserveLiveBuffer = false);
    // Keep live events behind a fence until all missing forward pages are installed.
    QJsonObject beginDaemonTranscriptCatchup(const QString& sessionId);
    void finishDaemonTranscriptCatchup(const QString& sessionId, const QJsonObject& page,
                                       ResponseCallback callback);
    void cancelDaemonTranscriptCatchup(const QString& sessionId);

    void call(const QString& method, const QJsonObject& params, ResponseCallback callback);
    void notify(const QString& method, const QJsonObject& params = {});
    void respondToUserInput(const QJsonValue& requestId, const QJsonObject& answers,
                            const QJsonArray& questions = {});
    void rejectUserInput(const QJsonValue& requestId, const QString& code, const QString& message);
    void prioritizeDaemonRestore(const QString& worktreePath, const QString& sessionId = {});
    // Persists the sidebar read marker for a daemon session (session.mark_seen).
    void executionWorktreeCommand(const QString& sessionId, const QString& type,
                                  const QJsonObject& payload, ResponseCallback callback);
    [[nodiscard]] qint64 daemonSessionCursor(const QString& sessionId, qint64 generation) const;
    void markDaemonSessionSeen(const QString& sessionId, qint64 cursor, ResponseCallback callback);
    [[nodiscard]] bool isDaemonSessionRestoring(const QString& sessionId) const;
    // Whether the session's installed transcript window still receives its live events:
    // nothing invalidated it and no window read or catch-up is replacing it.
    [[nodiscard]] bool isDaemonTranscriptWindowCurrent(const QString& sessionId) const;
    void stop();
    // Direct (non-daemon) mode: no active turn, no in-flight or queued call.
    [[nodiscard]] bool isIdle() const;
    [[nodiscard]] browser_tools::CodexLaunchReloader* browserToolsReloader() const noexcept {
        return reloader_;
    }
    // Direct mode: browser tool arguments the current Codex process was launched with. Only an
    // injected `ketplus_browser` server is auto-approved.
    [[nodiscard]] const QStringList& launchedBrowserArguments() const noexcept {
        return launchedBrowserArguments_;
    }
    // Tests: replaces the user-settings lookup that yields the browser tool launch arguments.
    void setBrowserArgumentsProvider(std::function<QStringList()> provider);
    static QVariantList permissionProfiles();
    static QVariantList permissionProfilesFor(const QString& provider);
    static QJsonObject permissionParams(const QString& profile, bool turn);

  signals:
    void readyChanged(bool ready);
    void notificationReceived(const QString& method, const QJsonObject& params);
    void userInputRequested(const QJsonValue& requestId, const QJsonObject& params);
    void serverStopped(const QString& reason);
    // Direct mode: an idle process was restarted to apply settings (reason "browser_settings").
    void serverReloaded(const QString& reason);
    void daemonSessionDiscovered(const QJsonObject& session);
    void executionContextChanged(const QString& sessionId, const QString& branch,
                                 const QString& executionPath, qint64 revision);
    void daemonSessionDeleted(const QString& sessionId, qint64 generation);
    void daemonSessionRestoreStarted(const QString& sessionId);
    void daemonSessionRestoreFinished(const QString& sessionId);
    void daemonTranscriptInvalidated(const QString& sessionId);
    // A replay (session.attach) failed. eventsPruned means the daemon reported cursor_expired:
    // the local transcript has a gap and must be reloaded rather than treated as restored.
    void daemonReplayFailed(const QString& sessionId, const QString& error, bool eventsPruned);
    void restoreStateChanged(bool restoring);

  private:
    struct QueuedCall final {
        QString method;
        QJsonObject params;
        ResponseCallback callback;
    };

    void ensureStarted();
    void initialize();
    void sendCall(const QString& method, const QJsonObject& params, ResponseCallback callback);
    void sendObject(const QJsonObject& object);
    void consumeStdout();
    void consumeLine(const QByteArray& line);
    void handleServerRequest(const QJsonObject& request);
    void failPending(const QString& reason);
    void setReady(bool ready);
    void setupProcessTransport();
    void setupDaemonTransport();
    void daemonCall(const QString& method, const QJsonObject& params, ResponseCallback callback);
    void refreshDaemonSessions();
    void queueDaemonDiscovery(const QJsonObject& envelope);
    void discoverDaemonSessions();
    void reorderDaemonRestoreQueue();
    void restoreNextDaemonSession();
    void attachDaemonSession(const QString& sessionId);
    void consumeDaemonEvent(const QJsonObject& envelope);
    void dispatchDaemonEvent(const QJsonObject& envelope);
    void resolveDaemonLargeEvents(const QString& sessionId, qint64 generation, quint64 epoch,
                                  const QJsonObject& page, ResponseCallback callback);
    void finishDaemonTranscriptWindow(const QString& sessionId, qint64 generation, quint64 epoch,
                                      bool initial, const QJsonObject& result, const QString& error,
                                      ResponseCallback callback);
    void finishDaemonReplay(const QString& sessionId);
    void dispatchDaemonApproval(const QString& sessionId, const QJsonObject& data);
    void setRestoring(bool restoring);
    void restartForBrowserSettings();
    [[nodiscard]] QStringList browserArguments() const;
    [[nodiscard]] static QString findCodexExecutable();

    QString executable_;
    QProcess* process_{nullptr};
    browser_tools::CodexLaunchReloader* reloader_{nullptr};
    std::function<QStringList()> browserArgumentsProvider_;
    QStringList launchedBrowserArguments_;
    QSet<QString> activeTurns_;
    QByteArray stdoutBuffer_;
    QByteArray stderrBuffer_;
    QHash<quint64, ResponseCallback> pending_;
    QList<QueuedCall> queued_;
    quint64 nextRequestId_{1};
    bool starting_{false};
    bool ready_{false};
    bool stopping_{false};
    bool restoring_{false};
    bool daemonSessionListInFlight_{false};
    bool daemonDiscoveryInFlight_{false};
    bool daemonDiscoveryScheduled_{false};
    quint64 daemonDiscoveryEpoch_{0};
    QHash<QString, QPair<qint64, qint64>> daemonDiscoveryCursors_;
    bool daemonHasByteBoundedReplay_{false};
    bool daemonHasTranscriptWindow_{false};
    bool daemonHasTranscriptAfterCursor_{false};
    int daemonTranscriptWindowMaxTurns_{3};
    bool daemonHasLargeEvents_{false};
    remote::DaemonClient* daemon_{nullptr};
    struct DaemonSession final {
        QString cwd;
        qint64 contextRevision{1};
        qint64 generation{1};
        qint64 cursor{0};
        qint64 replayCursor{0};
        qint64 windowCursor{0};
        qint64 deferredCursor{0};
        QJsonObject options;
        QMap<qint64, QJsonObject> deferredEvents;
        bool replaying{false};
        bool replayRequestInFlight{false};
        bool windowReady{false};
        bool windowOverflow{false};
        qsizetype deferredBytes{0};
        quint64 windowEpoch{0};
        bool approvalSyncInFlight{false};
        QSet<QString> settledApprovals;
        QString turnId;
        bool turnStartInFlight{false};
        bool interruptRequested{false};
    };
    QHash<QString, DaemonSession> daemonSessions_;
    QSet<QString> restoringDaemonSessions_;
    QStringList daemonRestoreQueue_;
    QString activeDaemonRestoreSession_;
    QString daemonRestorePriorityPath_;
    QString daemonRestorePrioritySession_;
    struct PendingDaemonApproval final {
        QString sessionId;
        QString kind;
        int revision{1};
        QJsonArray questions;
    };
    QHash<QString, PendingDaemonApproval> daemonApprovals_;
};

} // namespace ketplus
