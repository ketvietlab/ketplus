#include "exec/LinuxPolicy.h"
#include "exec/ExecLog.h"
#include "exec/ExecutionPlan.h"

#include <utility>

namespace ketplus::runner::exec {
namespace {

const QString systemdRun = QStringLiteral("/usr/bin/systemd-run");
const QString cgroupControllers = QStringLiteral("/sys/fs/cgroup/cgroup.controllers");

} // namespace

LinuxPolicy::LinuxPolicy(LinuxHost& host, QString program, QStringList runnerDirectories)
    : m_host(host), m_program(std::move(program)), m_directories(std::move(runnerDirectories)),
      m_alive(std::make_shared<bool>(true)) {}

// A probe still out answers no one.
LinuxPolicy::~LinuxPolicy() { *m_alive = false; }

void LinuxPolicy::answer(const std::function<void(bool, const QString&)>& done,
                         const char* failedStep) {
    if (failedStep)
        qCInfo(lcRunnerExec) << "containment check failed:" << failedStep;
    done(!failedStep, failedStep ? QStringLiteral("containment_unavailable") : QString());
}

void LinuxPolicy::probe(std::function<void(bool, const QString&)> done) {
    const uint uid = m_host.effectiveUid();
    if (uid == 0)
        return answer(done, "account");
    if (m_host.bootId().isEmpty())
        return answer(done, "boot_id");
    if (m_program.isEmpty() || !m_host.executable(m_program))
        return answer(done, "executor");
    const QString runtime = m_host.runtimeDirectory();
    if (!m_host.executable(systemdRun) || runtime.isEmpty() ||
        m_host.directoryOwner(runtime) != uid || !m_host.socket(runtime + QStringLiteral("/bus")))
        return answer(done, "user_session");
    if (!m_directories.isEmpty()) {
        QStringList real{m_host.realDirectory(runtime)};
        for (const QString& directory : std::as_const(m_directories))
            real.append(m_host.realDirectory(directory));
        if (real.contains(QString()) || directoriesNested(real))
            return answer(done, "runner_directories");
    }
    if (!m_host.file(cgroupControllers))
        return answer(done, "cgroup");

    const std::weak_ptr<bool> alive = m_alive;
    m_host.probeContainment(m_program, [this, alive, done](const bool contained) {
        const std::shared_ptr<bool> live = alive.lock();
        if (!live || !*live)
            return;
        if (!contained)
            return answer(done, "executor_probe");
        m_host.probeMounts(m_program, [this, alive, done](const bool mounted) {
            const std::shared_ptr<bool> live = alive.lock();
            if (!live || !*live)
                return;
            answer(done, mounted ? nullptr : "mount_probe");
        });
    });
}

} // namespace ketplus::runner::exec
