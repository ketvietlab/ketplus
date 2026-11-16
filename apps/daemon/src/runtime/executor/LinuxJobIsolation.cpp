#include "runtime/executor/LinuxJobContainment.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QTextStream>
#ifdef Q_OS_LINUX
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ketplus::daemon::executor {
#ifdef Q_OS_LINUX
namespace {
bool writeMap(const char* path, const QByteArray& data) {
    const int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool success = write(fd, data.constData(), size_t(data.size())) == data.size();
    close(fd);
    return success;
}
int isolationFailure(const char* reason) {
    dprintf(STDERR_FILENO, "containment_unavailable: %s (errno=%d)\n", reason, errno);
    return 125;
}
void closeAuthorityDescriptors() {
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3U, ~0U, 0) == 0)
        return;
#endif
    const long limit = sysconf(_SC_OPEN_MAX);
    // Fail closed rather than silently leaking a descriptor beyond our scan.
    if (limit < 0 || limit > 1048576)
        _exit(125);
    for (int fd = 3; fd < limit; ++fd)
        close(fd);
}
} // namespace
#endif

int linuxIsolationLaunch(const QJsonObject& execution, bool probe) {
#ifdef Q_OS_LINUX
    // This trusted launcher runs with the supervisor environment. Workload env
    // is applied only after namespace setup, so LD_PRELOAD cannot run before it.
    QProcessEnvironment environment;
    const auto inherited = execution.value("_base_environment").toObject();
    for (auto it = inherited.begin(); it != inherited.end(); ++it) {
        if (it.key().isEmpty() || it.key().contains('=') || it.key().contains(QChar::Null) ||
            !it.value().isString() || it.value().toString().contains(QChar::Null))
            return 125;
        environment.insert(it.key(), it.value().toString());
    }
    const auto delta = execution.value("env_delta").toObject();
    for (auto it = delta.begin(); it != delta.end(); ++it)
        if (it.value().isNull())
            environment.remove(it.key());
        else
            environment.insert(it.key(), it.value().toString());
    QList<QByteArray> environmentBytes, argumentBytes;
    for (const auto& item : environment.toStringList())
        environmentBytes.append(item.toLocal8Bit());
    for (const auto& arg : execution.value("argv").toArray())
        argumentBytes.append(arg.toString().toLocal8Bit());
    if (argumentBytes.isEmpty() && !execution.value("shell_command").toString().isEmpty())
        argumentBytes = {"/bin/sh", "-c",
                         execution.value("shell_command").toString().toLocal8Bit()};
    QList<char*> environmentPointers, argumentPointers;
    for (auto& item : environmentBytes)
        environmentPointers.append(item.data());
    environmentPointers.append(nullptr);
    for (auto& arg : argumentBytes)
        argumentPointers.append(arg.data());
    argumentPointers.append(nullptr);
    const auto uid = getuid(), gid = getgid();
    if (uid == 0 || unshare(CLONE_NEWUSER) != 0 || !writeMap("/proc/self/setgroups", "deny") ||
        !writeMap("/proc/self/uid_map", "0 " + QByteArray::number(uid) + " 1") ||
        !writeMap("/proc/self/gid_map", "0 " + QByteArray::number(gid) + " 1") ||
        unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWCGROUP) != 0 ||
        mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0)
        return isolationFailure("user/mount/PID/cgroup namespaces are required");
    const pid_t init = fork();
    if (init < 0)
        return isolationFailure("namespace init fork failed");
    if (init > 0) {
        // Keep the wrapper alive until namespace PID 1 exits. The kernel then
        // kills every remaining namespace member, including detached children.
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(init, &status, 0);
        } while (waited < 0 && errno == EINTR);
        return waited < 0 ? 125 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) != 0)
        _exit(isolationFailure("an isolated proc mount is required"));
    char mountpoint[] = "/tmp/ketplus-cgroup-view-XXXXXX";
    if (!mkdtemp(mountpoint))
        _exit(isolationFailure("a private cgroup view directory is required"));
    if (mount("none", mountpoint, "cgroup2", 0, nullptr) != 0)
        _exit(isolationFailure("a namespace-rooted cgroup mount is required"));
    if (mount(mountpoint, "/sys/fs/cgroup", nullptr, MS_BIND, nullptr) != 0)
        _exit(isolationFailure("an isolated cgroup view is required"));
    umount2(mountpoint, MNT_DETACH);
    rmdir(mountpoint);
    closeAuthorityDescriptors();
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        _exit(isolationFailure("no_new_privs failed"));
    if (probe) {
        const int fd = open("/proc/self/cgroup", O_RDONLY | O_CLOEXEC);
        char content[128]{};
        const ssize_t count = fd < 0 ? -1 : read(fd, content, sizeof(content) - 1);
        if (fd >= 0)
            close(fd);
        _exit(count > 0 && QByteArray(content, count).trimmed() == "0::/" && getpid() == 1 ? 0
                                                                                           : 125);
    }
    if (argumentBytes.isEmpty())
        _exit(125);
    // Only stdio remains; no cgroup migration FD, original namespace handle,
    // parent /proc alias, bus socket or root-directory FD crosses this exec.
    ::environ = environmentPointers.data();
    execvp(argumentPointers[0], argumentPointers.data());
    _exit(errno == ENOENT ? 127 : 126);
#else
    Q_UNUSED(execution)
    Q_UNUSED(probe)
    return 125;
#endif
}

int linuxContainmentProbe(const QString& task, const QString& lease) {
#ifdef Q_OS_LINUX
    QString error;
    const auto boundary = linuxCreateBoundary(task, lease, &error);
    bool available = !boundary.isEmpty();
    int gate[2]{-1, -1};
    if (available && pipe2(gate, O_CLOEXEC) == 0) {
        const pid_t child = fork();
        if (child == 0) {
            close(gate[1]);
            char token = 0;
            const auto received = read(gate[0], &token, 1);
            close(gate[0]);
            _exit(received == 1 && token == '1' ? linuxIsolationLaunch({}, true) : 125);
        }
        close(gate[0]);
        available = child > 0 && linuxPlaceLauncher(boundary, child);
        if (available)
            available = write(gate[1], "1", 1) == 1;
        close(gate[1]);
        int status = 0;
        if (child > 0) {
            if (!available)
                linuxKillBoundary(boundary);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
        }
        available = available && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                    linuxBoundaryEmpty(boundary);
    } else {
        available = false;
    }
    if (!boundary.isEmpty()) {
        linuxKillBoundary(boundary);
        QDir().rmdir(boundary.value("path").toString());
    }
    QTextStream(stdout)
        << QJsonDocument(QJsonObject{{"available", available}}).toJson(QJsonDocument::Compact)
        << Qt::endl;
    return available ? 0 : 125;
#else
    Q_UNUSED(task)
    Q_UNUSED(lease)
    return 125;
#endif
}
} // namespace ketplus::daemon::executor
