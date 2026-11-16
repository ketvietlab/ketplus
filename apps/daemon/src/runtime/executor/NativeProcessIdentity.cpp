#include "runtime/executor/NativeProcessIdentity.h"

#include <QVector>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <limits>
#include <unistd.h>

#ifdef Q_OS_MACOS
#include <libproc.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
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

bool signalIdentity(const NativeProcessIdentity& identity, int signal, QString* error) {
    if (identity.pid <= 0 || identity.birth.isEmpty()) {
        if (error)
            *error = "Missing process birth identity.";
        return false;
    }
    const auto current = inspect(identity.pid);
    if (!current) {
        if (kill(pid_t(identity.pid), 0) != 0 && errno == ESRCH)
            return true;
        if (error)
            *error = "Process identity is unavailable.";
        return false;
    }
    if (current->birth != identity.birth) {
        if (error)
            *error = "Process birth changed; the reused PID was not signalled.";
        return false;
    }
    if (current->zombie)
        return true;
    if (kill(pid_t(identity.pid), signal) == 0 || errno == ESRCH)
        return true;
    if (error)
        *error = QString::fromLocal8Bit(strerror(errno));
    return false;
}
} // namespace ketplus::daemon::executor
