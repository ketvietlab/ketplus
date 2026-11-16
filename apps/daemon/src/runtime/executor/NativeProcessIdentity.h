#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
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
[[nodiscard]] QList<qint64> children(qint64 pid, bool* complete);
[[nodiscard]] QList<qint64> groupMembers(qint64 group, bool* complete);
// Rechecks birth immediately before signalling. A reused PID is never intentionally adopted.
bool signalIdentity(const NativeProcessIdentity& identity, int signal, QString* error);
} // namespace ketplus::daemon::executor
