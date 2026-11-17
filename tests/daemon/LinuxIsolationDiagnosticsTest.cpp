// Fault injection only: every namespace/mount/fork call is intercepted, and
// successful setup stops at a simulated fork failure. No real probe is run.
#include "runtime/executor/LinuxJobContainment.h"

#include <QCoreApplication>
#include <QProcess>
#include <QTest>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <unistd.h>

namespace {
enum Failure {
    Root,
    UserNamespace,
    SetgroupsOpen,
    SetgroupsWrite,
    UidMapOpen,
    UidMapWrite,
    UidMapShort,
    GidMapOpen,
    GidMapWrite,
    OtherNamespaces,
    Propagation,
    InitFork
};
int injected = -1;
constexpr int mapFd = 10000;
int mapIndex(const char* path) {
    const char* paths[]{"/proc/self/setgroups", "/proc/self/uid_map", "/proc/self/gid_map"};
    for (int i = 0; i != 3; ++i)
        if (std::strcmp(path, paths[i]) == 0)
            return i;
    return -1;
}
int mockOpen(const char* path) {
    const int index = mapIndex(path);
    if (index < 0 || (index == 0 && injected == SetgroupsOpen) ||
        (index == 1 && injected == UidMapOpen) || (index == 2 && injected == GidMapOpen)) {
        errno = EACCES;
        return -1;
    }
    return mapFd + index;
}
} // namespace

extern "C" {
uid_t __real_getuid();
gid_t __real_getgid();
int __real_unshare(int);
int __real_open(const char*, int, ...);
int __real_open64(const char*, int, ...);
ssize_t __real_write(int, const void*, size_t);
int __real_close(int);
int __real_mount(const char*, const char*, const char*, unsigned long, const void*);
pid_t __real_fork();

uid_t __wrap_getuid() { return injected < 0 ? __real_getuid() : injected == Root ? 0 : 1001; }
gid_t __wrap_getgid() { return injected < 0 ? __real_getgid() : 1001; }
int __wrap_unshare(int flags) {
    if (injected < 0)
        return __real_unshare(flags);
    if ((flags == CLONE_NEWUSER && injected == UserNamespace) ||
        (flags != CLONE_NEWUSER && injected == OtherNamespaces)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}
int __wrap_open(const char* path, int flags, ...) {
    if (injected >= 0)
        return mockOpen(path);
    va_list args;
    va_start(args, flags);
    const bool needsMode = (flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE;
    const auto mode = needsMode ? va_arg(args, mode_t) : mode_t{};
    va_end(args);
    return __real_open(path, flags, mode);
}
int __wrap_open64(const char* path, int flags, ...) {
    if (injected >= 0)
        return mockOpen(path);
    va_list args;
    va_start(args, flags);
    const bool needsMode = (flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE;
    const auto mode = needsMode ? va_arg(args, mode_t) : mode_t{};
    va_end(args);
    return __real_open64(path, flags, mode);
}
ssize_t __wrap_write(int fd, const void* data, size_t size) {
    if (injected < 0 || fd < mapFd || fd > mapFd + 2)
        return __real_write(fd, data, size);
    if ((fd == mapFd && injected == SetgroupsWrite) ||
        (fd == mapFd + 1 && injected == UidMapWrite) ||
        (fd == mapFd + 2 && injected == GidMapWrite)) {
        errno = EACCES;
        return -1;
    }
    return ssize_t(size) - (fd == mapFd + 1 && injected == UidMapShort ? 1 : 0);
}
int __wrap_close(int fd) {
    if (injected < 0 || fd < mapFd || fd > mapFd + 2)
        return __real_close(fd);
    errno = EBADF; // Deliberately clobber errno after a failed or short write.
    return 0;
}
int __wrap_mount(const char* source, const char* target, const char* type, unsigned long flags,
                 const void* data) {
    if (injected < 0)
        return __real_mount(source, target, type, flags, data);
    if (injected == Propagation) {
        errno = EACCES;
        return -1;
    }
    return 0;
}
pid_t __wrap_fork() {
    if (injected < 0)
        return __real_fork();
    errno = EAGAIN;
    return -1;
}
} // extern "C"

class LinuxIsolationDiagnosticsTest final : public QObject {
    Q_OBJECT
  private slots:
    void firstFailureIsIdentifiedWithoutChangingAdmission_data() {
        QTest::addColumn<int>("failure");
        QTest::addColumn<QByteArray>("stage");
        QTest::addColumn<QByteArray>("operation");
        QTest::addColumn<int>("error");
        const auto row = [](const char* name, Failure failure, const char* stage,
                            const char* operation, int error = EACCES) {
            QTest::newRow(name) << int(failure) << QByteArray(stage) << QByteArray(operation)
                                << error;
        };
        row("root", Root, "caller_uid", "require_non_root", EPERM);
        row("user-namespace", UserNamespace, "user_namespace", "unshare");
        row("setgroups-open", SetgroupsOpen, "setgroups", "open");
        row("setgroups-write", SetgroupsWrite, "setgroups", "write");
        row("uid-map-open", UidMapOpen, "uid_map", "open");
        row("uid-map-write", UidMapWrite, "uid_map", "write");
        row("uid-map-short-write", UidMapShort, "uid_map", "write", EIO);
        row("gid-map-open", GidMapOpen, "gid_map", "open");
        row("gid-map-write", GidMapWrite, "gid_map", "write");
        row("other-namespaces", OtherNamespaces, "mount_pid_cgroup_namespaces", "unshare");
        row("mount-propagation", Propagation, "mount_propagation", "mount");
        row("namespace-init", InitFork, "namespace_init", "fork", EAGAIN);
    }
    void firstFailureIsIdentifiedWithoutChangingAdmission() {
        QFETCH(int, failure);
        QFETCH(QByteArray, stage);
        QFETCH(QByteArray, operation);
        QFETCH(int, error);
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(),
                    {"--mock-isolation", QString::number(failure)});
        QVERIFY(child.waitForFinished(5000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 125);
        const auto diagnostic = child.readAllStandardError();
        QCOMPARE(diagnostic.count("containment_unavailable:"), 1);
        QVERIFY2(diagnostic.contains("stage=" + stage + " op=" + operation),
                 diagnostic.constData());
        QVERIFY2(diagnostic.contains("(errno=" + QByteArray::number(error) + ')'),
                 diagnostic.constData());
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--mock-isolation") {
        bool valid = false;
        const int failure = app.arguments()[2].toInt(&valid);
        if (!valid || failure < Root || failure > InitFork)
            return 64;
        injected = failure;
        errno = EBADF; // Root refusal must not report a stale syscall's errno.
        return ketplus::daemon::executor::linuxIsolationLaunch({}, true);
    }
    LinuxIsolationDiagnosticsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "LinuxIsolationDiagnosticsTest.moc"
