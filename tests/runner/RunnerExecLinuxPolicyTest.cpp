#include "RunnerExecTest.h"

#include "exec/LinuxPolicy.h"

#include <QLoggingCategory>
#include <QTest>

#include <map>

// The Linux policy over a fake machine: each check that fails answers containment_unavailable,
// in order, naming only its step; a machine that recovers is ready at the next probe.
namespace runner_exec_test {
namespace {

const QString program = QStringLiteral("/opt/runner/ketplus_job_executor");
const QString runtime = QStringLiteral("/run/user/1000");

class FakeLinuxHost final : public LinuxHost {
  public:
    uint effectiveUid() const override { return uid; }
    QString bootId() const override { return boot; }
    bool executable(const QString& path) const override { return executables.contains(path); }
    QString runtimeDirectory() const override { return runtimeDir; }
    std::optional<uint> directoryOwner(const QString& path) const override {
        const auto it = owners.find(path);
        return it == owners.end() ? std::nullopt : std::optional<uint>(it->second);
    }
    bool socket(const QString& path) const override { return sockets.contains(path); }
    bool file(const QString& path) const override { return files.contains(path); }
    void probeContainment(const QString& probed, std::function<void(bool)> done) override {
        containmentProbes.append(probed);
        pendingContainment.append(std::move(done));
    }
    void probeMounts(const QString& probed, std::function<void(bool)> done) override {
        mountProbes.append(probed);
        pendingMounts.append(std::move(done));
    }

    uint uid = 1000;
    QString boot = QStringLiteral("boot-1");
    QStringList executables{program, QStringLiteral("/usr/bin/systemd-run")};
    QString runtimeDir = runtime;
    std::map<QString, uint> owners{{runtime, 1000}};
    QStringList sockets{runtime + QStringLiteral("/bus")};
    QStringList files{QStringLiteral("/sys/fs/cgroup/cgroup.controllers")};
    QStringList containmentProbes;
    QStringList mountProbes;
    QList<std::function<void(bool)>> pendingContainment;
    QList<std::function<void(bool)>> pendingMounts;
};

struct Answer {
    int calls = 0;
    bool available = false;
    QString code;
    std::function<void(bool, const QString&)> callback() {
        return [this](const bool isAvailable, const QString& answered) {
            ++calls;
            available = isAvailable;
            code = answered;
        };
    }
};

QStringList logged;
void capture(QtMsgType, const QMessageLogContext&, const QString& message) {
    logged.append(message);
}

} // namespace

void RunnerExecLinuxPolicyTest::readyWhenEveryCheckHolds() {
    FakeLinuxHost host;
    LinuxPolicy policy(host, program);
    Answer answer;
    policy.probe(answer.callback());
    QCOMPARE(answer.calls, 0);
    QCOMPARE(host.containmentProbes, QStringList{program});
    host.pendingContainment.takeFirst()(true);
    QCOMPARE(host.mountProbes, QStringList{program});
    QCOMPARE(answer.calls, 0);
    host.pendingMounts.takeFirst()(true);
    QCOMPARE(answer.calls, 1);
    QVERIFY(answer.available);
    QVERIFY(answer.code.isEmpty());
}

void RunnerExecLinuxPolicyTest::eachFailedCheckIsUnavailable_data() {
    QTest::addColumn<QString>("broken");
    QTest::addColumn<QString>("step");
    QTest::newRow("root") << "root" << "account";
    QTest::newRow("no boot id") << "boot" << "boot_id";
    QTest::newRow("no executor") << "executor" << "executor";
    QTest::newRow("no systemd-run") << "systemd-run" << "user_session";
    QTest::newRow("no runtime directory") << "runtime" << "user_session";
    QTest::newRow("runtime of another user") << "owner" << "user_session";
    QTest::newRow("runtime not a directory") << "not-dir" << "user_session";
    QTest::newRow("no user bus") << "bus" << "user_session";
    QTest::newRow("no cgroup v2") << "cgroup" << "cgroup";
    QTest::newRow("executor probe fails") << "containment" << "executor_probe";
    QTest::newRow("mount probe fails") << "mounts" << "mount_probe";
}

void RunnerExecLinuxPolicyTest::eachFailedCheckIsUnavailable() {
    QFETCH(QString, broken);
    QFETCH(QString, step);
    FakeLinuxHost host;
    if (broken == "root")
        host.uid = 0;
    else if (broken == "boot")
        host.boot.clear();
    else if (broken == "executor")
        host.executables.removeAll(program);
    else if (broken == "systemd-run")
        host.executables.removeAll(QStringLiteral("/usr/bin/systemd-run"));
    else if (broken == "runtime")
        host.runtimeDir.clear();
    else if (broken == "owner")
        host.owners[runtime] = 1001;
    else if (broken == "not-dir")
        host.owners.clear();
    else if (broken == "bus")
        host.sockets.clear();
    else if (broken == "cgroup")
        host.files.clear();

    QLoggingCategory::setFilterRules(QStringLiteral("ketplus.runner.exec*=true"));
    logged.clear();
    const QtMessageHandler previous = qInstallMessageHandler(capture);
    LinuxPolicy policy(host, program);
    Answer answer;
    policy.probe(answer.callback());
    if (!host.pendingContainment.isEmpty())
        host.pendingContainment.takeFirst()(broken != "containment");
    if (!host.pendingMounts.isEmpty())
        host.pendingMounts.takeFirst()(broken != "mounts");
    qInstallMessageHandler(previous);

    QCOMPARE(answer.calls, 1);
    QVERIFY(!answer.available);
    QCOMPARE(answer.code, QStringLiteral("containment_unavailable"));
    // The probes run only once every local check holds, and in order.
    const bool local = broken != "containment" && broken != "mounts";
    QCOMPARE(host.containmentProbes.isEmpty(), local);
    QCOMPARE(host.mountProbes.isEmpty(), broken != "mounts");
    QCOMPARE(logged.size(), 1);
    QVERIFY(logged.first().endsWith(step));
    for (const QString& message : logged)
        QVERIFY(!message.contains(program) && !message.contains(runtime));
}

// The runner probes again later: a machine fixed meanwhile is ready, through the command runner.
void RunnerExecLinuxPolicyTest::recoveryIsReadyAtTheNextProbe() {
    FakeLinuxHost host;
    host.sockets.clear();
    LinuxPolicy policy(host, program);
    QList<Readiness> seen;
    ExecutorCommandRunner::Options options;
    options.directory = QStringLiteral("/nonexistent/executor");
    options.unavailableProbeMs = 20;
    FakeExecutor executor;
    ExecutorCommandRunner::Events events;
    events.readinessChanged = [&seen](const Readiness& next) { seen.append(next); };
    ExecutorCommandRunner runner(options, executor, policy, events);
    runner.probe();
    QCOMPARE(runner.readiness(),
             (Readiness{Readiness::State::Unavailable, "containment_unavailable"}));
    host.sockets = {runtime + QStringLiteral("/bus")};
    QTRY_COMPARE(host.pendingContainment.size(), 1);
    host.pendingContainment.takeFirst()(true);
    host.pendingMounts.takeFirst()(true);
    QVERIFY(runner.admit());
    QCOMPARE(seen.size(), 2);
}

// A probe answered after the policy is gone reaches no one.
void RunnerExecLinuxPolicyTest::lateAnswerAfterDestruction() {
    FakeLinuxHost host;
    Answer answer;
    {
        LinuxPolicy policy(host, program);
        policy.probe(answer.callback());
    }
    host.pendingContainment.takeFirst()(true);
    QVERIFY(host.mountProbes.isEmpty());
    QCOMPARE(answer.calls, 0);
}

void RunnerExecLinuxPolicyTest::closedPolicyStaysClosed() {
    ClosedPolicy policy;
    Answer answer;
    policy.probe(answer.callback());
    QCOMPARE(answer.calls, 1);
    QVERIFY(!answer.available);
    QCOMPARE(answer.code, QStringLiteral("containment_unavailable"));
}

} // namespace runner_exec_test
