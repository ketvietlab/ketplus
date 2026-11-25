#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <memory>
#include <optional>

namespace ketplus::daemon::executor {
struct NativeProcessIdentity final {
    qint64 pid{0};
    qint64 parent{0};
    qint64 group{0};
    QString birth;
    bool zombie{false};
    [[nodiscard]] QJsonObject json() const;
};

[[nodiscard]] std::optional<NativeProcessIdentity> inspect(qint64 pid);
// Why inspect() found nothing. Owned: inspect() succeeds. Gone: the OS reports no such
// process. Foreign: a process of another user holds the PID (identity carries its birth, so
// a reused PID is told apart from the original that changed user). Unknown: cannot tell.
enum class ProcessPresence { Owned, Gone, Foreign, Unknown };
struct ProcessProbe final {
    ProcessPresence presence{ProcessPresence::Unknown};
    std::optional<NativeProcessIdentity> identity;
};
[[nodiscard]] ProcessProbe probe(qint64 pid);
// Whether a recorded process that inspect() can no longer read may still be alive beyond our
// control. Gone, or a PID that another user's process reuses (a different birth), proves the
// original exited; an unchanged birth under another user, or an unreadable PID that kill(0)
// still reports, cannot be shown stopped and stays an alarm (process_identity_lost).
// existsByKill: kill(pid, 0) did not fail with ESRCH.
[[nodiscard]] bool originalMayBeAlive(const NativeProcessIdentity& known, const ProcessProbe& seen,
                                      bool existsByKill);
[[nodiscard]] QList<qint64> children(qint64 pid, bool* complete);
[[nodiscard]] QList<qint64> groupMembers(qint64 group, bool* complete);
enum class GroupState { Empty, Occupied, Unknown };
// Ask the OS whether the recorded job process group still has a live member. Zombies
// do not count. Anything that cannot be listed or inspected completely is Unknown,
// never Empty: only Empty may stand in for verified cleanup.
[[nodiscard]] GroupState inspectGroup(qint64 group);
// Capture a kernel-bound signal target before validating its recorded birth.
// A later send cannot target a different PID incarnation, even after exec/reuse.
class NativeProcessSignal final {
  public:
    static std::unique_ptr<NativeProcessSignal> capture(const NativeProcessIdentity& identity,
                                                        QString* error);
    ~NativeProcessSignal();
    NativeProcessSignal(const NativeProcessSignal&) = delete;
    NativeProcessSignal& operator=(const NativeProcessSignal&) = delete;
    bool send(int signal, QString* error) const;

  private:
    NativeProcessSignal() = default;
    int descriptor_{-1};
    QByteArray auditToken_;
    bool gone_{false};
};

bool signalIdentity(const NativeProcessIdentity& identity, int signal, QString* error);
} // namespace ketplus::daemon::executor
