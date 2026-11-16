#include "runtime/executor/NativeProcessIdentity.h"

#include <QFile>
#include <QFileInfo>
#include <QVector>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <limits>
#include <unistd.h>

#ifdef Q_OS_MACOS
#include <libproc.h>
#include <mach/mach.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#elif defined(Q_OS_LINUX)
#include <sys/syscall.h>
#endif

namespace ketplus::daemon::executor {
QJsonObject NativeProcessIdentity::json() const {
    return {{"pid", pid}, {"parent_pid", parent}, {"process_group", group}, {"birth", birth}};
}

std::optional<NativeProcessIdentity> inspect(qint64 pid) {
    if (pid <= 0 || pid > std::numeric_limits<pid_t>::max())
        return {};
#ifdef Q_OS_MACOS
    proc_bsdinfo info{};
    if (proc_pidinfo(int(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != int(sizeof(info)) ||
        info.pbi_uid != geteuid())
        return {};
    return NativeProcessIdentity{pid, info.pbi_ppid, info.pbi_pgid,
                                 QString::number(info.pbi_start_tvsec) + ":" +
                                     QString::number(info.pbi_start_tvusec),
                                 info.pbi_status == SZOMB};
#elif defined(Q_OS_LINUX)
    const auto path = "/proc/" + QString::number(pid);
    if (QFileInfo(path).ownerId() != geteuid())
        return {};
    QFile stat(path + "/stat"), boot("/proc/sys/kernel/random/boot_id");
    if (!stat.open(QIODevice::ReadOnly) || !boot.open(QIODevice::ReadOnly))
        return {};
    const auto bytes = stat.read(8192);
    const auto close = bytes.lastIndexOf(')');
    const auto fields = bytes.mid(close + 2).simplified().split(' ');
    if (close < 0 || fields.size() < 20 || fields[19].isEmpty())
        return {};
    return NativeProcessIdentity{pid, fields[1].toLongLong(), fields[2].toLongLong(),
                                 QString::fromLatin1(boot.read(128).trimmed()) + ':' +
                                     QString::fromLatin1(fields[19]),
                                 fields[0] == "Z"};
#else
    return {};
#endif
}

namespace {
#ifdef Q_OS_MACOS
QList<qint64> listed(qint64 id, bool* complete, int (*list)(pid_t, void*, int)) {
    *complete = false;
    const int needed = list(pid_t(id), nullptr, 0);
    if (needed < 0)
        return {};
    constexpr int maximumEntries = 65536;
    // libproc takes a byte-sized buffer but returns a count of PIDs, including
    // for its sizing query. Treating that count as bytes silently drops children.
    const int capacity = qMin(maximumEntries, needed + 64);
    QVector<pid_t> buffer(capacity);
    const int count = list(pid_t(id), buffer.data(), capacity * int(sizeof(pid_t)));
    if (count < 0)
        return {};
    *complete = count < capacity;
    QList<qint64> result;
    for (int i = 0; i < qMin(count, capacity); ++i)
        if (buffer[i] > 0)
            result.append(buffer[i]);
    return result;
}
#endif
} // namespace

QList<qint64> children(qint64 pid, bool* complete) {
#ifdef Q_OS_MACOS
    return listed(pid, complete, proc_listchildpids);
#else
    Q_UNUSED(pid)
    *complete = false;
    return {};
#endif
}
QList<qint64> groupMembers(qint64 group, bool* complete) {
#ifdef Q_OS_MACOS
    return listed(group, complete, proc_listpgrppids);
#else
    Q_UNUSED(group)
    *complete = false;
    return {};
#endif
}

std::unique_ptr<NativeProcessSignal>
NativeProcessSignal::capture(const NativeProcessIdentity& identity, QString* error) {
    if (identity.pid <= 0 || identity.pid > std::numeric_limits<pid_t>::max() ||
        identity.birth.isEmpty()) {
        if (error)
            *error = "Missing process birth identity.";
        return {};
    }
    auto target = std::unique_ptr<NativeProcessSignal>(new NativeProcessSignal);
#ifdef Q_OS_LINUX
    target->descriptor_ = int(syscall(SYS_pidfd_open, pid_t(identity.pid), 0));
    if (target->descriptor_ < 0) {
        if (errno == ESRCH) {
            target->gone_ = true;
            return target;
        }
        if (error)
            *error = "A verified pidfd is unavailable.";
        return {};
    }
#elif defined(Q_OS_MACOS)
    mach_port_t port = MACH_PORT_NULL;
    auto result = task_name_for_pid(mach_task_self(), int(identity.pid), &port);
    audit_token_t token{};
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    if (result == KERN_SUCCESS)
        result = task_info(port, TASK_AUDIT_TOKEN, reinterpret_cast<task_info_t>(&token), &count);
    if (port != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), port);
    if (result != KERN_SUCCESS || count != TASK_AUDIT_TOKEN_COUNT || token.val[5] != identity.pid) {
        const auto current = inspect(identity.pid);
        if (current && current->birth == identity.birth && current->zombie) {
            target->gone_ = true;
            return target;
        }
        if (kill(pid_t(identity.pid), 0) != 0 && errno == ESRCH) {
            target->gone_ = true;
            return target;
        }
        if (error)
            *error = "A verified process audit token is unavailable.";
        return {};
    }
    target->auditToken_ = QByteArray(reinterpret_cast<const char*>(&token), sizeof(token));
#else
    if (error)
        *error = "Identity-bound signaling is unavailable on this platform.";
    return {};
#endif
    // The kernel target is captured FIRST. A process exiting or execing after
    // this check cannot redirect the eventual signal to a new incarnation.
    const auto current = inspect(identity.pid);
    if (!current) {
        if (kill(pid_t(identity.pid), 0) != 0 && errno == ESRCH) {
            target->gone_ = true;
            return target;
        }
        if (error)
            *error = "Process identity is unavailable.";
        return {};
    }
    if (current->birth != identity.birth) {
        if (error)
            *error = "Process birth changed; the reused PID was not signalled.";
        return {};
    }
    target->gone_ = current->zombie;
    return target;
}

NativeProcessSignal::~NativeProcessSignal() {
    if (descriptor_ >= 0)
        close(descriptor_);
}

bool NativeProcessSignal::send(int signal, QString* error) const {
    if (gone_)
        return true;
    int result = ENOTSUP;
#ifdef Q_OS_LINUX
    result = syscall(SYS_pidfd_send_signal, descriptor_, signal, nullptr, 0) == 0 ? 0 : errno;
#elif defined(Q_OS_MACOS)
    audit_token_t token{};
    if (auditToken_.size() != sizeof(token))
        return false;
    memcpy(&token, auditToken_.constData(), sizeof(token));
    // libproc returns the error number directly, rather than setting errno.
    result = proc_signal_with_audittoken(&token, signal);
#endif
    if (result == 0 || result == ESRCH)
        return true;
    if (error)
        *error = QString::fromLocal8Bit(strerror(result));
    return false;
}

bool signalIdentity(const NativeProcessIdentity& identity, int signal, QString* error) {
    const auto target = NativeProcessSignal::capture(identity, error);
    return target && target->send(signal, error);
}
} // namespace ketplus::daemon::executor
