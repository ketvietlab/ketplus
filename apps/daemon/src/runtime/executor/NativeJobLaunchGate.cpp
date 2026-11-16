#include "runtime/executor/NativeJobSupervisor.h"

#include <QSocketNotifier>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace ketplus::daemon::executor {
void NativeJobSupervisor::closeLaunchGate() {
    delete launchReady_;
    launchReady_ = nullptr;
    for (auto* pair : {launchIdentityPipe_, launchGate_})
        for (int index = 0; index < 2; ++index)
            if (pair[index] >= 0) {
                close(pair[index]);
                pair[index] = -1;
            }
}

bool NativeJobSupervisor::prepareLaunchGate(QString* error) {
    if (pipe(launchIdentityPipe_) != 0 || pipe(launchGate_) != 0) {
        closeLaunchGate();
        *error = "Unable to create the durable pre-exec identity gate.";
        return false;
    }
    for (auto* pair : {launchIdentityPipe_, launchGate_})
        for (int index = 0; index < 2; ++index)
            if (fcntl(pair[index], F_SETFD, FD_CLOEXEC) < 0) {
                closeLaunchGate();
                *error = "Unable to isolate the pre-exec identity descriptors.";
                return false;
            }
    const int identityRead = launchIdentityPipe_[0], identityWrite = launchIdentityPipe_[1];
    const int gateRead = launchGate_[0], gateWrite = launchGate_[1];
    process_.setChildProcessModifier([this, identityRead, identityWrite, gateRead, gateWrite] {
        // Only async-signal-safe syscalls and Qt's startup-failure helper are
        // permitted here; the latter reports through the startup pipe and exits.
        close(identityRead);
        close(gateWrite);
        if (setsid() < 0)
            process_.failChildProcessModifier("Unable to establish the process group", errno);
        const pid_t identity = getpid();
        ssize_t written;
        do {
            written = write(identityWrite, &identity, sizeof(identity));
        } while (written < 0 && errno == EINTR);
        close(identityWrite);
        if (written != sizeof(identity))
            process_.failChildProcessModifier("The identity gate closed before registration");
        char proceed = 0;
        ssize_t received;
        do {
            received = read(gateRead, &proceed, 1);
        } while (received < 0 && errno == EINTR);
        close(gateRead);
        if (received != 1 || proceed != '1')
            process_.failChildProcessModifier("The pre-exec identity gate was not committed");
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGPIPE, &action, nullptr) != 0)
            process_.failChildProcessModifier("Unable to reset workload signal handling", errno);
    });
    launchReady_ = new QSocketNotifier(identityRead, QSocketNotifier::Read, this);
    connect(launchReady_, &QSocketNotifier::activated, this, [this] { commitLaunchIdentity(); });
    return true;
}

void NativeJobSupervisor::commitLaunchIdentity() {
    launchReady_->setEnabled(false);
    pid_t pid = 0;
    ssize_t received;
    do {
        received = read(launchIdentityPipe_[0], &pid, sizeof(pid));
    } while (received < 0 && errno == EINTR);
    const auto identity =
        received == sizeof(pid) && pid == process_.processId() ? inspect(pid) : std::nullopt;
    if (identity && !identity->zombie && identity->group == pid && !stopping_) {
        root_ = identity;
        known_.insert(pid, *identity);
        metadata_.insert("root_identity", identity->json());
        metadata_.insert("executor_stopped", false);
        metadata_.insert("launch_gate_committed", true);
        if (persist()) {
            const char proceed = '1';
            ssize_t written;
            do {
                written = write(launchGate_[1], &proceed, 1);
            } while (written < 0 && errno == EINTR);
            if (written != 1)
                metadata_.insert("error", "handshake_failed");
        }
    } else {
        metadata_.insert("error", "handshake_failed");
    }
    close(launchGate_[1]);
    launchGate_[1] = -1;
    close(launchIdentityPipe_[0]);
    launchIdentityPipe_[0] = -1;
}
} // namespace ketplus::daemon::executor
