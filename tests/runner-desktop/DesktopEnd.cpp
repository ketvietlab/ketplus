#include "DesktopEnd.h"

#include "cloud-privacy/CloudPrivacyTestAccess.h"
#include "cloud/CloudControlClient.h"
#include "pairing/PairingIdentity.h"
#include "providers/CodexAdapter.h"
#include "runner/RunnerPinStore.h"
#include "runner/RunnerService.h"
#include "runner/entry/RemoteJobEntry.h"
#include "runner/jobs/PinRunnerTrust.h"
#include "runner/jobs/RemoteJobRelay.h"
#include "runner/link/LinkAdapters.h"
#include "runner/link/RunnerLinks.h"
#include "runner/output/TaskOutputSpool.h"
#include "runner/peer/QtJobPeerClock.h"
#include "runner/remote/SnapshotStores.h"
#include "runner/sync/SnapshotKeys.h"
#include "runner/sync/SnapshotPreparation.h"
#include "runtime/SessionHost.h"
#include "storage/BackgroundTaskOutput.h"
#include "storage/LocalJournal.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace runner_desktop_test {

using Access = ketplus::daemon::CloudPrivacyTestAccess;
namespace daemon = ketplus::daemon;
namespace runner = ketplus::daemon::runner;

// One real WebRTC transport between a runner link and the WebRTC stack, which the test can
// lose: the stack closes and the link hears closed(), as after a failed ICE or DTLS path.
class LosableTransports::Transport final : public link::LinkTransport,
                                           public link::LinkTransportEvents {
  public:
    Transport(LosableTransports& owner, link::LinkTransportEvents& events)
        : m_owner(owner), m_events(events) {
        m_real = owner.m_real->create(*this);
        m_owner.m_live.append(this);
    }
    ~Transport() override { m_owner.m_live.removeOne(this); }

    bool answer(const QString& offerSdp, const QJsonArray& iceServers) override {
        return !m_lost && m_real->answer(offerSdp, iceServers);
    }
    bool candidate(const QString& candidate, const QString& mid) override {
        return !m_lost && m_real->candidate(candidate, mid);
    }
    bool send(const link::TransportChannel channel, const QByteArray& bytes,
              const bool binary) override {
        return !m_lost && m_real->send(channel, bytes, binary);
    }
    [[nodiscard]] quint64 buffered(const link::TransportChannel channel) const override {
        return m_lost ? 0 : m_real->buffered(channel);
    }
    void close() override {
        if (!m_lost)
            m_real->close();
    }

    void localDescription(const QString& sdp) override {
        if (!m_lost)
            m_events.localDescription(sdp);
    }
    void localCandidate(const QString& candidate, const QString& mid) override {
        if (!m_lost)
            m_events.localCandidate(candidate, mid);
    }
    void opened() override {
        if (m_lost)
            return;
        m_open = true;
        m_events.opened();
    }
    void received(const link::TransportChannel channel, const QByteArray& bytes,
                  const bool binary) override {
        if (!m_lost)
            m_events.received(channel, bytes, binary);
    }
    void writable() override {
        if (!m_lost)
            m_events.writable();
    }
    void closed() override {
        if (!m_lost)
            m_events.closed();
    }

    [[nodiscard]] bool open() const { return m_open && !m_lost; }
    // Nothing more passes either way; the link may delete this transport in closed().
    void lose() {
        m_lost = true;
        m_real->close();
        m_events.closed();
    }

  private:
    LosableTransports& m_owner;
    link::LinkTransportEvents& m_events;
    std::unique_ptr<link::LinkTransport> m_real;
    bool m_open = false;
    bool m_lost = false;
};

LosableTransports::LosableTransports()
    : m_real(std::make_unique<link::WebRtcAnswerTransportFactory>()) {}

LosableTransports::~LosableTransports() = default;

std::unique_ptr<link::LinkTransport> LosableTransports::create(link::LinkTransportEvents& events) {
    return std::make_unique<Transport>(*this, events);
}

int LosableTransports::loseOpen() {
    int lost = 0;
    const QList<Transport*> live = m_live;
    for (Transport* transport : live) {
        if (!m_live.contains(transport) || !transport->open())
            continue;
        transport->lose();
        ++lost;
    }
    return lost;
}

LoopbackOnly::LoopbackOnly(const quint16 port) : m_port(port) { setProxy(QNetworkProxy::NoProxy); }

QNetworkReply* LoopbackOnly::createRequest(const Operation operation,
                                           const QNetworkRequest& request, QIODevice* outgoing) {
    const QUrl url = request.url();
    if (url.scheme() != QStringLiteral("http") || url.host() != QStringLiteral("127.0.0.1") ||
        url.port() != m_port) {
        // Counted and sent nowhere: the test fails on any refusal.
        ++m_refused;
        return QNetworkAccessManager::createRequest(
            operation, QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:1/refused"))),
            outgoing);
    }
    return QNetworkAccessManager::createRequest(operation, request, outgoing);
}

// Signaling of the runner links leaves on the daemon's Relay socket.
class DesktopEnd::Signals final : public link::SignalOut {
  public:
    explicit Signals(daemon::CloudControlClient& cloud) : m_cloud(cloud) {}
    void sendSignal(const QJsonObject& message) override {
        (void)m_cloud.sendRelayMessage(message);
    }

  private:
    daemon::CloudControlClient& m_cloud;
};

DesktopEnd::DesktopEnd(const QUrl& relay, const QString& root, const QString& worktree)
    : m_root(root) {
    // The Relay socket's address is read once, when the client is made.
    qputenv("KETPLUS_CONTROL_URL", relay.toString().toUtf8());
    QDir().mkpath(root);
    m_journal = std::make_unique<daemon::LocalJournal>(QDir(root).filePath("journal.sqlite"));
    m_registry.emplace(m_journal->backgroundTasks());
    QString error;
    if (!daemon::BackgroundTaskOutput(m_registry->database()).initialize(&error))
        qFatal("Output store cannot start");
    // A conversation the user asked to approve each task of; its provider never starts.
    daemon::StoredSession session;
    session.id = sessionId;
    session.worktreePath = worktree;
    session.adapter = QStringLiteral("codex");
    session.state = QStringLiteral("ready");
    session.executionPolicy = QStringLiteral("ask");
    if (!m_journal->upsertSession(session))
        qFatal("Session cannot be stored");
    m_provider =
        std::make_unique<daemon::CodexAdapter>(QStringLiteral("/nonexistent/synthetic-provider"));
    m_host = std::make_unique<daemon::SessionHost>(m_journal.get(), m_provider.get());
    m_network = std::make_unique<LoopbackOnly>(quint16(relay.port()));
    m_client = std::make_unique<daemon::CloudControlClient>(m_journal.get(), m_host.get());
    Access::configure(*m_client, *m_network, relay);
    Access::machineKeyVersion(*m_client, 1);
    const QJsonArray capabilities{"transport.webrtc/1", "runner.relay/1"};
    Access::heartbeatAnswer(*m_client,
                            {{"compatibility", QJsonObject{{"capabilities", capabilities}}}});

    // As RemoteJobsService composes it, with the transports the test can lose.
    runner::RunnerRelayPort& port = m_client->runners().port();
    m_pins = std::make_unique<runner::RunnerPinStore>(port.credentials());
    m_relay = std::make_unique<runner::jobs::RemoteJobRelay>(port);
    m_jobClock = std::make_unique<SkippableJobClock>();
    m_trust = std::make_unique<runner::jobs::PinRunnerTrust>(*m_pins);
    m_peerClock = std::make_unique<runner::peer::QtJobPeerClock>(
        [this](const QString& jobId, const runner::peer::PeerTimer timer) {
            m_jobs->peerTimer(jobId, timer);
        });
    const QString snapshots = QDir(root).filePath(QStringLiteral("runner-snapshots"));
    m_stores = std::make_unique<runner::remote::SnapshotStores>(
        QDir(snapshots).filePath(QStringLiteral("stores")),
        QDir(snapshots).filePath(QStringLiteral("packs")));
    m_secret = std::make_unique<runner::sync::RunnerSecret>(port.credentials());
    m_linkClock = std::make_unique<link::QtLinkClock>();
    m_linkPins = std::make_unique<link::FunctionRunnerPins>(
        [pins = m_pins.get()](const QString& runnerId) { return pins->pin(runnerId); });
    m_signals = std::make_unique<Signals>(*m_client);

    runner::remote::RemoteJobsPorts ports;
    ports.database = m_registry->database();
    ports.ledger = &*m_registry;
    ports.relay = m_relay.get();
    ports.jobClock = m_jobClock.get();
    ports.trust = m_trust.get();
    ports.peerClock = m_peerClock.get();
    ports.stores = m_stores.get();
    ports.daemonVersion = QCoreApplication::applicationVersion();
    ports.relayAvailable = [&port] { return port.runnerRelayAvailable(); };
    ports.accountScope = [&port] { return port.accountScope(); };
    ports.gitProblem = runner::sync::checkGit();
    ports.secret = [secret = m_secret.get()] { return secret->obtain(); };
    ports.prepare = [](runner::sync::PrepareRequest request,
                       std::function<void(runner::sync::PreparedOutcome)> done) {
        QThreadPool::globalInstance()->start(
            [request = std::move(request), done = std::move(done)]() mutable {
                runner::sync::PreparedOutcome outcome = runner::sync::prepareSnapshot(request);
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [outcome = std::move(outcome), done = std::move(done)] { done(outcome); },
                    Qt::QueuedConnection);
            });
    };
    ports.makeLinks = [this,
                       &port](link::LinkEvents& events) -> std::unique_ptr<link::RunnerLinks> {
        const std::optional<runner::MachineIdentity> identity = port.machineIdentity();
        if (!identity)
            return nullptr;
        m_identity = std::make_unique<link::FunctionMachineIdentity>(
            identity->workspaceId, identity->machineId, identity->keyVersion,
            [&port](const QByteArray& bytes) { return port.signAsMachine(bytes); });
        return std::make_unique<link::RunnerLinks>(*m_signals, *m_linkClock, m_transports,
                                                   *m_identity, *m_linkPins, events);
    };
    ports.openSpool = [database = m_registry->database()](
                          const QString& taskId) -> std::unique_ptr<runner::peer::OutputSpool> {
        return runner::output::TaskOutputSpool::open(database, taskId);
    };
    ports.defer = [](const qint64 delayMs, std::function<void()> work) {
        QTimer::singleShot(std::max<qint64>(0, delayMs), QCoreApplication::instance(),
                           std::move(work));
    };
    m_jobs = std::make_unique<runner::remote::RemoteJobs>(std::move(ports));
    m_jobs->setChanged([this](const runner::jobs::RemoteJobRecord& record) {
        m_changes.append(record.taskId + ' ' + record.state + ' ' + record.placement);
    });
    m_jobClock->setFired([this](const QString& taskId, const runner::jobs::JobTimer timer) {
        m_jobs->jobTimer(taskId, timer);
    });
    m_linkClock->setFired([this](const QString& connectionId) { m_jobs->linkTimer(connectionId); });
    QObject::connect(&m_client->runners(), &runner::RunnerService::runnerRemoved,
                     &m_client->runners(),
                     [this](const QString& runnerId, const QString& clientId) {
                         (void)m_jobs->runnerRemoved(runnerId, clientId);
                     });

    // RemoteJobsService::start, then the client's own Relay socket.
    m_jobs->restore();
    m_client->setRunnerMessages(
        [this](const QJsonObject& message) { return m_jobs->relayMessage(message); },
        [this] { m_jobs->relaySocketReopened(); });
    m_jobs->sweep(QDateTime::currentDateTimeUtc());
    m_entry = std::make_unique<runner::entry::RemoteJobEntry>(*m_jobs);
    m_host->setRemoteJobs(m_entry.get());
    Access::wakeups(*m_client).start();

    const QString packs = QDir(snapshots).filePath(QStringLiteral("packs"));
    QObject::connect(&m_packWatch, &QTimer::timeout, [this, packs] {
        const QDir directory(packs);
        for (const QString& name : directory.entryList({QStringLiteral("*.json")}, QDir::Files)) {
            QFile file(directory.filePath(name));
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const QJsonObject side = QJsonDocument::fromJson(file.readAll()).object();
            if (!side.value(QStringLiteral("jobId")).toString().isEmpty())
                m_packs.insert(side.value(QStringLiteral("jobId")).toString(), side);
        }
    });
    m_packWatch.start(50);
}

DesktopEnd::~DesktopEnd() {
    m_packWatch.stop();
    Access::wakeups(*m_client).stop();
    m_client->setRunnerMessages({}, {});
    m_jobClock->setFired({});
    m_linkClock->setFired({});
    m_host->setRemoteJobs(nullptr);
    m_entry.reset();
    m_jobs.reset();
    // Snapshot work still on the pool answers into nothing once the service is gone.
    QThreadPool::globalInstance()->waitForDone();
    QCoreApplication::processEvents();
}

bool DesktopEnd::pinned(const QString& runnerId) const { return m_pins->pin(runnerId).has_value(); }

bool DesktopEnd::socketOpen() const { return Access::wakeups(*m_client).isOpen(); }

QByteArray DesktopEnd::signingPublicKey() const {
    const QJsonObject keys = Access::crypto(*m_client).publicKeys();
    return ketplus::runner::pairing::decodeBase64Url(keys.value("signingPublicKey").toString())
        .value_or(QByteArray());
}

QByteArray DesktopEnd::agreementPublicKey() const {
    const QJsonObject keys = Access::crypto(*m_client).publicKeys();
    return ketplus::runner::pairing::decodeBase64Url(keys.value("agreementPublicKey").toString())
        .value_or(QByteArray());
}

QJsonObject DesktopEnd::call(const QString& type, const QJsonObject& payload) {
    QJsonObject result;
    bool done = false;
    m_client->handleLocal(type, payload, [&](const QJsonObject& value, const QString& error) {
        result = value;
        if (!error.isEmpty())
            result.insert("transport_error", error);
        done = true;
    });
    if (!QTest::qWaitFor([&] { return done; }, 10'000))
        return {{"ok", false}, {"error_code", "test_timeout"}};
    return result;
}

QJsonObject DesktopEnd::command(const QString& type, const QJsonObject& payload) {
    QJsonObject result;
    bool done = false;
    m_host->handleCommandDeferred({{"message_id", QUuid::createUuid().toString()},
                                   {"message_type", type},
                                   {"session_id", sessionId},
                                   {"session_generation", 1},
                                   {"payload", payload}},
                                  QStringLiteral("local-test"), [&](const QJsonObject& reply) {
                                      result = reply;
                                      done = true;
                                  });
    if (!QTest::qWaitFor([&] { return done; }, 10'000))
        return {{"ok", false}, {"error_code", "test_timeout"}};
    return result;
}

QString DesktopEnd::submit(const QString& shell, const QString& cwd, const QStringList& runsOn,
                           const qint64 timeoutMs, const QMap<QString, QString>& env,
                           QString* code) {
    QJsonObject delta;
    for (auto entry = env.cbegin(); entry != env.cend(); ++entry)
        delta.insert(entry.key(), entry.value());
    const QJsonObject started = command(
        QStringLiteral("session.task.start"),
        {{"client_request_id", QStringLiteral("e2e-%1").arg(++m_requests)},
         {"kind", "process"},
         {"title", "e2e"},
         {"timeout_ms", timeoutMs},
         {"runs_on", QJsonArray::fromStringList(runsOn)},
         {"execution_spec",
          QJsonObject{{"explicit_shell_command", shell}, {"cwd", cwd}, {"env_delta", delta}}}});
    const QString taskId = started.value("task").toObject().value("task_id").toString();
    const auto answer = [code](const QString& value) {
        if (code)
            *code = value;
    };
    if (taskId.isEmpty()) {
        answer(started.value("error_code").toString());
        return {};
    }
    // The user approves the task the host holds for approval; then it goes to the remote jobs.
    QJsonArray pending;
    if (!QTest::qWaitFor(
            [&] {
                pending = command(QStringLiteral("session.pending_approvals"), {})
                              .value("pending_approvals")
                              .toArray();
                return !pending.isEmpty();
            },
            10'000)) {
        answer(QStringLiteral("no_approval"));
        return taskId;
    }
    const QJsonObject resolved =
        command(QStringLiteral("approval.resolve"),
                {{"approval_id", pending.first().toObject().value("approval_id")},
                 {"resolution", QJsonObject{{"decision", "accept"}}}});
    if (!resolved.value("ok").toBool()) {
        answer(resolved.value("error_code").toString(QStringLiteral("approval_refused")));
        return taskId;
    }
    if (!QTest::qWaitFor([&] { return m_jobs->record(taskId).has_value(); }, 60'000))
        answer(QStringLiteral("no_record"));
    else
        answer({});
    return taskId;
}

QString DesktopEnd::stop(const QString& taskId) {
    const QJsonObject reply = command(QStringLiteral("session.task.stop"), {{"task_id", taskId}});
    return reply.value("ok").toBool() ? QString() : reply.value("error_code").toString();
}

QString DesktopEnd::resolveCleanup(const QString& taskId) {
    const QJsonObject task =
        command(QStringLiteral("session.task.get"), {{"task_id", taskId}}).value("task").toObject();
    const QJsonObject reply =
        command(QStringLiteral("session.task.resolve_cleanup"),
                {{"task_id", taskId}, {"revision", task.value("revision").toInteger(-1)}});
    return reply.value("ok").toBool() ? QString() : reply.value("error_code").toString();
}

std::optional<runner::jobs::RemoteJobRecord> DesktopEnd::record(const QString& taskId) const {
    return m_jobs->record(taskId);
}

QByteArray DesktopEnd::output(const QString& taskId) const {
    const daemon::BackgroundTaskOutput store(m_registry->database());
    QByteArray bytes;
    QString cursor;
    for (int page = 0; page < 1000; ++page) {
        const daemon::BackgroundOutputPage read = store.read(taskId, cursor);
        bytes += read.bytes;
        if (!read.error.isEmpty() || read.bytes.isEmpty() || read.nextCursor == cursor)
            break;
        cursor = read.nextCursor;
    }
    return bytes;
}

QString DesktopEnd::taskState(const QString& taskId) const {
    return m_registry->runtimeTask(taskId).value("state").toString();
}

} // namespace runner_desktop_test
