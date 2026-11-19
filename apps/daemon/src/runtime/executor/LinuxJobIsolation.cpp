#include "runtime/executor/LinuxJobContainment.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QTextStream>
#ifdef Q_OS_LINUX
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
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

// The job's mounts, from the execution's optional `mounts` object: each `hidden` directory is
// covered by a small read-only tmpfs, the job's `tmp` directory replaces /tmp, and `workspace` is
// mounted again at its own path. Every path is absolute, a real directory and not a symbolic
// link; none of them may overlap in a way that would leave a hidden directory visible.
struct JobMounts {
    QList<QByteArray> hidden;
    QByteArray workspace;
    QByteArray tmp;
    QByteArray cwd;
    int workspaceFd = -1;
    int tmpFd = -1;
};

constexpr qsizetype maxHidden = 16;
const char* const hiddenOptions = "size=64k,nr_inodes=128,mode=0755";

bool realDirectoryPath(const QJsonValue& value, QByteArray* out) {
    const QString path = value.toString();
    if (!value.isString() || !path.startsWith('/') || path == "/" || path.contains(QChar::Null))
        return false;
    const QByteArray bytes = QFile::encodeName(path);
    char resolved[PATH_MAX];
    struct stat info{};
    if (!realpath(bytes.constData(), resolved) || bytes != resolved || stat(resolved, &info) != 0 ||
        !S_ISDIR(info.st_mode))
        return false;
    *out = bytes;
    return true;
}

bool within(const QByteArray& path, const QByteArray& root) {
    return path == root || path.startsWith(root + '/');
}

const char* readJobMounts(const QJsonObject& execution, JobMounts* mounts) {
    const QJsonObject object = execution.value("mounts").toObject();
    const QJsonArray hidden = object.value("hidden").toArray();
    if (!execution.value("mounts").isObject() || object.size() != 3 || hidden.isEmpty() ||
        hidden.size() > maxHidden)
        return "mounts are malformed";
    for (const auto& value : hidden) {
        QByteArray path;
        if (!realDirectoryPath(value, &path))
            return "a hidden directory is not a real directory";
        mounts->hidden.append(path);
    }
    if (!realDirectoryPath(object.value("workspace"), &mounts->workspace) ||
        !realDirectoryPath(object.value("tmp"), &mounts->tmp))
        return "the workspace or tmp directory is not a real directory";
    mounts->cwd = QFile::encodeName(execution.value("cwd").toString());
    if (!within(mounts->cwd, mounts->workspace))
        return "the working directory is outside the workspace";
    if (within(mounts->workspace, mounts->tmp) || within(mounts->tmp, mounts->workspace))
        return "the workspace and tmp directories overlap";
    for (qsizetype i = 0; i < mounts->hidden.size(); ++i) {
        const QByteArray& path = mounts->hidden[i];
        if (within(path, mounts->workspace) || within(path, mounts->tmp) || within("/tmp", path))
            return "a hidden directory overlaps the workspace, tmp or /tmp";
        for (qsizetype j = 0; j < mounts->hidden.size(); ++j)
            if (i != j && within(path, mounts->hidden[j]))
                return "hidden directories overlap";
    }
    return nullptr;
}

// Opened before any covering mount, so each directory is reached whatever covers its path later.
bool openJobMounts(JobMounts* mounts) {
    const int flags = O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    mounts->workspaceFd = open(mounts->workspace.constData(), flags);
    mounts->tmpFd = open(mounts->tmp.constData(), flags);
    return mounts->workspaceFd >= 0 && mounts->tmpFd >= 0;
}

QByteArray descriptorPath(int fd) { return "/proc/self/fd/" + QByteArray::number(fd); }

// The flags a bind remount must keep: those of the file system the directory lives on.
unsigned long lockedFlags(int fd) {
    struct statvfs info{};
    if (fstatvfs(fd, &info) != 0)
        return MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC;
    unsigned long flags = 0;
    if (info.f_flag & ST_RDONLY)
        flags |= MS_RDONLY;
    if (info.f_flag & ST_NOSUID)
        flags |= MS_NOSUID;
    if (info.f_flag & ST_NODEV)
        flags |= MS_NODEV;
    if (info.f_flag & ST_NOEXEC)
        flags |= MS_NOEXEC;
    if (info.f_flag & ST_NOATIME)
        flags |= MS_NOATIME;
    if (info.f_flag & ST_NODIRATIME)
        flags |= MS_NODIRATIME;
    if (info.f_flag & ST_RELATIME)
        flags |= MS_RELATIME;
    return flags;
}

bool makeMountPoint(const QByteArray& path) {
    for (qsizetype slash = path.indexOf('/', 1);; slash = path.indexOf('/', slash + 1)) {
        const QByteArray prefix = slash < 0 ? path : path.left(slash);
        if (mkdir(prefix.constData(), 0755) != 0 && errno != EEXIST)
            return false;
        if (slash < 0)
            return true;
    }
}

const char* applyJobMounts(const JobMounts& mounts) {
    if (mount(descriptorPath(mounts.tmpFd).constData(), "/tmp", nullptr, MS_BIND, nullptr) != 0 ||
        mount(nullptr, "/tmp", nullptr,
              MS_REMOUNT | MS_BIND | MS_NOSUID | MS_NODEV | lockedFlags(mounts.tmpFd),
              nullptr) != 0)
        return "the job tmp directory could not replace /tmp";
    QList<QByteArray> covered;
    for (const QByteArray& path : mounts.hidden) {
        struct stat info{};
        // A hidden directory under /tmp is already gone with the host's /tmp.
        if (within(path, "/tmp") && stat(path.constData(), &info) != 0 && errno == ENOENT)
            continue;
        if (mount("tmpfs", path.constData(), "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC,
                  hiddenOptions) != 0)
            return "a hidden directory could not be covered";
        covered.append(path);
    }
    if (!makeMountPoint(mounts.workspace) ||
        mount(descriptorPath(mounts.workspaceFd).constData(), mounts.workspace.constData(), nullptr,
              MS_BIND, nullptr) != 0)
        return "the workspace could not be mounted again";
    for (const QByteArray& path : covered)
        if (mount(nullptr, path.constData(), nullptr,
                  MS_REMOUNT | MS_BIND | MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC,
                  nullptr) != 0)
            return "a hidden directory could not be made read-only";
    return nullptr;
}

// A user and mount namespace below the one that made the mounts: the kernel locks every mount
// copied into it, so the command, root in the new namespace, cannot unmount what covers a hidden
// directory or make it writable again, nor from any namespace it creates later.
const char* lockJobMounts(const JobMounts& mounts) {
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0 || !writeMap("/proc/self/setgroups", "deny") ||
        !writeMap("/proc/self/uid_map", "0 0 1") || !writeMap("/proc/self/gid_map", "0 0 1"))
        return "a locking user and mount namespace is required";
    // The working directory is entered again through the new mounts.
    if (chdir(mounts.cwd.constData()) != 0)
        return "the working directory is not reachable through the mounts";
    return nullptr;
}

// Seen from inside, as a probe: the hidden directory is empty and cannot be uncovered, the
// workspace and the job's tmp are there.
bool jobMountsHold(const JobMounts& mounts) {
    const QByteArray hidden = mounts.hidden.first();
    struct stat info{};
    return stat((hidden + "/secret").constData(), &info) != 0 && errno == ENOENT &&
           stat((mounts.workspace + "/marker").constData(), &info) == 0 &&
           stat("/tmp/marker", &info) == 0 && umount2(hidden.constData(), MNT_DETACH) != 0 &&
           mkdir((hidden + "/written").constData(), 0700) != 0;
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
    const bool withMounts = execution.contains("mounts");
    JobMounts mounts;
    if (withMounts) {
        if (const char* invalid = readJobMounts(execution, &mounts))
            return isolationFailure(invalid);
    }
    const auto uid = getuid(), gid = getgid();
    if (uid == 0 || unshare(CLONE_NEWUSER) != 0 || !writeMap("/proc/self/setgroups", "deny") ||
        !writeMap("/proc/self/uid_map", "0 " + QByteArray::number(uid) + " 1") ||
        !writeMap("/proc/self/gid_map", "0 " + QByteArray::number(gid) + " 1") ||
        unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWCGROUP) != 0 ||
        mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0)
        return isolationFailure("user/mount/PID/cgroup namespaces are required");
    if (withMounts && !openJobMounts(&mounts))
        return isolationFailure("the workspace and tmp directories must open");
    const pid_t init = fork();
    if (init < 0)
        return isolationFailure("namespace init fork failed");
    if (init > 0) {
        if (withMounts) {
            close(mounts.workspaceFd);
            close(mounts.tmpFd);
        }
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
    if (withMounts) {
        if (const char* failed = applyJobMounts(mounts))
            _exit(isolationFailure(failed));
        if (const char* failed = lockJobMounts(mounts))
            _exit(isolationFailure(failed));
    }
    closeAuthorityDescriptors();
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        _exit(isolationFailure("no_new_privs failed"));
    if (probe) {
        const int fd = open("/proc/self/cgroup", O_RDONLY | O_CLOEXEC);
        char content[128]{};
        const ssize_t count = fd < 0 ? -1 : read(fd, content, sizeof(content) - 1);
        if (fd >= 0)
            close(fd);
        const bool held = !withMounts || jobMountsHold(mounts);
        _exit(held && count > 0 && QByteArray(content, count).trimmed() == "0::/" && getpid() == 1
                  ? 0
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

#ifdef Q_OS_LINUX
namespace {
// Directories for the mount probe, in the user's runtime directory when there is one: a hidden
// directory holding a secret and the workspace, and a tmp directory; each with a marker.
QJsonObject mountProbeExecution(const QString& base) {
    const QString hidden = base + "/hidden";
    const QString workspace = hidden + "/ws";
    const QString tmp = base + "/tmp";
    if (!QDir().mkpath(workspace) || !QDir().mkpath(tmp))
        return {};
    for (const QString& file : {hidden + "/secret", workspace + "/marker", tmp + "/marker"}) {
        QFile marker(file);
        if (!marker.open(QIODevice::WriteOnly))
            return {};
    }
    return {{"cwd", workspace},
            {"mounts",
             QJsonObject{{"hidden", QJsonArray{hidden}}, {"workspace", workspace}, {"tmp", tmp}}}};
}
} // namespace
#endif

int linuxContainmentProbe(const QString& task, const QString& lease) {
    return linuxContainmentProbe(task, lease, false);
}

int linuxContainmentProbe(const QString& task, const QString& lease, bool mounts) {
#ifdef Q_OS_LINUX
    QJsonObject execution;
    QString probeBase;
    if (mounts) {
        const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
        QByteArray pattern =
            QFile::encodeName((runtime.startsWith('/') ? runtime : QStringLiteral("/tmp")) +
                              "/ketplus-mount-probe-XXXXXX");
        if (mkdtemp(pattern.data())) {
            probeBase = QFile::decodeName(pattern);
            execution = mountProbeExecution(QFileInfo(probeBase).canonicalFilePath());
        }
    }
    QString error;
    const auto boundary = linuxCreateBoundary(task, lease, &error);
    bool available = !boundary.isEmpty() && (!mounts || !execution.isEmpty());
    int gate[2]{-1, -1};
    if (available && pipe2(gate, O_CLOEXEC) == 0) {
        const pid_t child = fork();
        if (child == 0) {
            close(gate[1]);
            char token = 0;
            const auto received = read(gate[0], &token, 1);
            close(gate[0]);
            _exit(received == 1 && token == '1' ? linuxIsolationLaunch(execution, true) : 125);
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
        available = linuxProbeSucceeded(available, status, [&boundary] {
            return linuxBoundaryEmptyWithin(boundary, linuxProbeEmptyDeadlineMs);
        });
    } else {
        available = false;
    }
    if (!boundary.isEmpty()) {
        linuxKillBoundary(boundary);
        QDir().rmdir(boundary.value("path").toString());
    }
    if (!probeBase.isEmpty()) {
        // The mounts stayed inside: the secret is still there from outside.
        available = available && QFileInfo::exists(probeBase + "/hidden/secret");
        QDir(probeBase).removeRecursively();
    }
    QTextStream(stdout)
        << QJsonDocument(QJsonObject{{"available", available}}).toJson(QJsonDocument::Compact)
        << Qt::endl;
    return available ? 0 : 125;
#else
    Q_UNUSED(task)
    Q_UNUSED(lease)
    Q_UNUSED(mounts)
    return 125;
#endif
}
} // namespace ketplus::daemon::executor
