#pragma once

#include "SkippableJobClock.h"
#include "runner/jobs/RemoteJobRecord.h"
#include "runner/link/LinkPorts.h"
#include "runner/remote/RemoteJobs.h"
#include "storage/BackgroundTaskLedger.h"

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QNetworkAccessManager>
#include <QString>
#include <QTimer>

#include <memory>
#include <optional>

namespace ketplus::daemon {
class CloudControlClient;
class CodexAdapter;
class LocalJournal;
class SessionHost;
namespace runner {
class RunnerPinStore;
namespace entry {
class RemoteJobEntry;
}
namespace jobs {
class PinRunnerTrust;
class RemoteJobRelay;
} // namespace jobs
namespace link {
class FunctionMachineIdentity;
class FunctionRunnerPins;
class QtLinkClock;
class WebRtcAnswerTransportFactory;
} // namespace link
namespace peer {
class QtJobPeerClock;
}
namespace sync {
class RunnerSecret;
}
namespace remote {
class SnapshotStores;
}
} // namespace runner
} // namespace ketplus::daemon

namespace runner_desktop_test {

namespace link = ketplus::daemon::runner::link;

// The real WebRTC answering transports, which the test can lose as a failing ICE or DTLS path
// would: the transport closes without signal.end and the links hear closed().
class LosableTransports final : public link::LinkTransportFactory {
  public:
    LosableTransports();
    ~LosableTransports() override;
    [[nodiscard]] std::unique_ptr<link::LinkTransport>
    create(link::LinkTransportEvents& events) override;
    // Loses every transport whose channels opened; answers how many.
    int loseOpen();

    class Transport;

  private:
    std::unique_ptr<link::WebRtcAnswerTransportFactory> m_real;
    QList<Transport*> m_live;
};

// Requests leave only for the loopback Relay of the test.
class LoopbackOnly final : public QNetworkAccessManager {
  public:
    explicit LoopbackOnly(quint16 port);
    [[nodiscard]] int refused() const { return m_refused; }

  protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* outgoing) override;

  private:
    quint16 m_port;
    int m_refused = 0;
};

// The desktop daemon's real parts for remote jobs, composed as the daemon composes them: its
// session host and the background task registry of its journal, the remote job entry the host
// starts tasks with runs_on through, its Control client and runner routes, runner pairing and
// pins, the remote job service with the output store and snapshot stores in a temporary
// directory, the runner links over WebRTC, and the Relay socket. Only the Relay they talk to is
// the test's.
class DesktopEnd {
  public:
    static inline const QString machineId = QStringLiteral("machine-synthetic");
    static inline const QString workspaceId = QStringLiteral("workspace-synthetic");
    static inline const QString machineToken = QStringLiteral("synthetic-test-token");
    // The conversation the test's tasks belong to.
    static inline const QString sessionId = QStringLiteral("e2e-session");

    // Its state in `root`; the test conversation's workspace, where its tasks run, is `worktree`.
    DesktopEnd(const QUrl& relay, const QString& root, const QString& worktree);
    ~DesktopEnd();
    DesktopEnd(const DesktopEnd&) = delete;
    DesktopEnd& operator=(const DesktopEnd&) = delete;

    // The desktop's machine keys, as the Relay knows them.
    [[nodiscard]] QByteArray signingPublicKey() const;
    [[nodiscard]] QByteArray agreementPublicKey() const;

    // A local IPC operation of the daemon (runner.*), answered or {"error_code":"test_timeout"}.
    QJsonObject call(const QString& type, const QJsonObject& payload = {});

    // A process task started with runs_on through the session host, running the shell command
    // `shell` in `cwd`, and approved by the user; empty when the host refuses it, with its
    // error code (or the step that did not happen) in `code`.
    QString submit(const QString& shell, const QString& cwd, const QStringList& runsOn,
                   qint64 timeoutMs, const QMap<QString, QString>& env, QString* code);
    // session.task.stop and session.task.resolve_cleanup of the local user: the error code, or
    // empty when the host took it.
    QString stop(const QString& taskId);
    QString resolveCleanup(const QString& taskId);
    [[nodiscard]] std::optional<ketplus::daemon::runner::jobs::RemoteJobRecord>
    record(const QString& taskId) const;
    [[nodiscard]] QByteArray output(const QString& taskId) const;
    [[nodiscard]] QString taskState(const QString& taskId) const;

    // Moves the clock of the desktop's job records `ms` forward; its timers due by then fire.
    void skipJobTime(qint64 ms) { m_jobClock->skip(ms); }
    // The daemon's own Relay socket is open.
    [[nodiscard]] bool socketOpen() const;
    [[nodiscard]] int refusedRequests() const { return m_network->refused(); }
    // Every record change the service reported, in order: "taskId state placement".
    [[nodiscard]] const QStringList& changes() const { return m_changes; }
    LosableTransports& transports() { return m_transports; }
    // The runner is pinned on this desktop.
    [[nodiscard]] bool pinned(const QString& runnerId) const;
    // The side file of every snapshot pack seen in the pack cache, by task id; read every 50 ms.
    [[nodiscard]] const QMap<QString, QJsonObject>& packsSeen() const { return m_packs; }

  private:
    class Signals;

    // One command of the local user to the session host, answered or {"error_code":...}.
    QJsonObject command(const QString& type, const QJsonObject& payload);

    QString m_root;
    std::unique_ptr<ketplus::daemon::LocalJournal> m_journal;
    std::optional<ketplus::daemon::BackgroundTaskLedger> m_registry;
    int m_requests = 0;
    std::unique_ptr<ketplus::daemon::CodexAdapter> m_provider;
    std::unique_ptr<ketplus::daemon::SessionHost> m_host;
    std::unique_ptr<LoopbackOnly> m_network;
    std::unique_ptr<ketplus::daemon::CloudControlClient> m_client;
    std::unique_ptr<ketplus::daemon::runner::RunnerPinStore> m_pins;
    std::unique_ptr<ketplus::daemon::runner::jobs::RemoteJobRelay> m_relay;
    std::unique_ptr<SkippableJobClock> m_jobClock;
    std::unique_ptr<ketplus::daemon::runner::jobs::PinRunnerTrust> m_trust;
    std::unique_ptr<ketplus::daemon::runner::peer::QtJobPeerClock> m_peerClock;
    std::unique_ptr<ketplus::daemon::runner::remote::SnapshotStores> m_stores;
    std::unique_ptr<ketplus::daemon::runner::sync::RunnerSecret> m_secret;
    std::unique_ptr<link::QtLinkClock> m_linkClock;
    LosableTransports m_transports;
    std::unique_ptr<link::FunctionRunnerPins> m_linkPins;
    std::unique_ptr<link::FunctionMachineIdentity> m_identity; // of the current links
    std::unique_ptr<Signals> m_signals;
    QStringList m_changes;
    QTimer m_packWatch;
    QMap<QString, QJsonObject> m_packs;
    // Last, so they go first: the entry before the remote jobs it holds.
    std::unique_ptr<ketplus::daemon::runner::remote::RemoteJobs> m_jobs;
    std::unique_ptr<ketplus::daemon::runner::entry::RemoteJobEntry> m_entry;
};

} // namespace runner_desktop_test
