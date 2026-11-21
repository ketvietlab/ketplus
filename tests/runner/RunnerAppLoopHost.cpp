#include "RunnerAppTest.h"

#include "run/AccessTokens.h"
#include "run/RunnerAssembly.h"
#include "run/RunnerLoop.h"
#include "state/RunnerState.h"
#include "state/StateDirectory.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QTimer>

// `run` over its production wiring with the time of the test host: the timers fire in order of
// their due time, and the host's clock jumps to each one. The Relay socket says ready as soon as
// it opens; no job comes, so no peer transport is ever made.
namespace runner_app_test {

using namespace ketplus::runner;
using namespace ketplus::runner::app;

namespace {

class HostHub final : public TimerHub {
  public:
    explicit HostHub(TestHost& host) : m_host(host) {}
    [[nodiscard]] QDateTime now() const override { return m_host.clock; }
    void arm(const TimerKey key, const qint64 delayMs) override {
        m_due.insert(code(key), {key, m_host.clock.addMSecs(delayMs)});
        if (key.domain == TimerDomain::Run && key.id == static_cast<int>(RunTimer::Heartbeat)) {
            m_host.sleeps.append(delayMs);
            if (m_host.sleeps.size() >= m_host.stopAfterSleeps)
                m_host.stop = true;
        }
    }
    void disarm(const TimerKey key) override { m_due.remove(code(key)); }
    void onFired(Fired fired) override { m_fired = std::move(fired); }

    bool fireNext() {
        auto next = m_due.cend();
        for (auto it = m_due.cbegin(); it != m_due.cend(); ++it)
            if (next == m_due.cend() || it->second < next->second)
                next = it;
        if (next == m_due.cend())
            return false;
        const auto [key, due] = *next;
        m_due.erase(next);
        if (due > m_host.clock)
            m_host.clock = due;
        if (m_fired)
            m_fired(key);
        return true;
    }

  private:
    [[nodiscard]] static int code(TimerKey key) {
        return static_cast<int>(key.domain) * 64 + key.id;
    }
    TestHost& m_host;
    Fired m_fired;
    QMap<int, std::pair<TimerKey, QDateTime>> m_due;
};

class ReadySocket final : public peer::RelaySocket {
  public:
    explicit ReadySocket(peer::RelaySocketEvents& events) : m_events(events) {}
    void open(const QString&) override {
        QTimer::singleShot(0, &m_home, [this] {
            m_events.socketOpened();
            m_events.socketText(QStringLiteral(R"({"type":"ready","role":"runner"})"));
        });
    }
    bool send(const QJsonObject&) override { return true; }
    void close() override {}

  private:
    peer::RelaySocketEvents& m_events;
    QObject m_home;
};

class NoTransports final : public peer::PeerTransportFactory {
  public:
    std::unique_ptr<peer::PeerTransport> create(peer::PeerTransportEvents&) override {
        qFatal("no job comes in these tests");
    }
};

void settle(RunnerLoop& loop) {
    for (int round = 0; round < 1000; ++round) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        if (loop.queue().idle())
            return;
    }
}

} // namespace

int TestHost::runLoop(const int sleepLimit) {
    sleeps.clear();
    stop = false;
    stopAfterSleeps = sleepLimit;
    const Environment env = environment();
    QString error;
    const std::optional<StateDirectory> state = StateDirectory::open(stateDir(), &error);
    if (!state) {
        err.append(error);
        return toInt(ExitCode::State);
    }
    const RunnerState runnerState(*state);
    std::optional<RunnerIdentity> identity = runnerState.loadIdentity(&error);
    if (!identity) {
        err.append(error);
        return toInt(ExitCode::State);
    }
    RelayClient client(env.relayUrl, env.requestTimeoutMs);
    AccessTokens tokens(client, runnerState, *identity, env);
    PairingContext context{env, client, runnerState, tokens};
    HostHub hub(*this);
    NoTransports transports;
    ExitCode failure = ExitCode::Failure;
    const std::unique_ptr<RunnerAssembly> assembly =
        RunnerAssembly::create(context, *identity, *state, workDir(),
                               RunSeams{hub,
                                        [](peer::RelaySocketEvents& events) {
                                            return std::make_unique<ReadySocket>(events);
                                        },
                                        transports, [this] { return stop ? 1 : 0; }},
                               &failure);
    if (!assembly)
        return toInt(failure);
    RunnerLoop& loop = assembly->loop();
    std::optional<ExitCode> exit;
    loop.start([&exit](const ExitCode code) { exit = code; });
    for (int fired = 0; !exit && fired < 100'000; ++fired) {
        settle(loop);
        if (exit || !hub.fireNext())
            break;
    }
    settle(loop);
    return toInt(exit.value_or(ExitCode::Failure));
}

} // namespace runner_app_test
