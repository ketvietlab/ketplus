#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>
#include <optional>

namespace ketplus::daemon {
struct BackgroundTaskIntent;
}

// What the background task runtime asks of remote jobs. A task with runs_on never runs on this
// machine; without a port such a task is refused before it exists.
namespace ketplus::daemon::runner::entry {

class RemoteJobPort {
  public:
    struct Ports {
        // Ends a reserved task that never got a record: failed with `code`, never launched.
        // Ignored by the runtime when the task already ended.
        std::function<void(const QString& taskId, const QString& code)> failReserved;
        // A remote record was stored: the runtime reads the task again and publishes it.
        std::function<void(const QString& taskId)> changed;
    };

    virtual ~RemoteJobPort() = default;

    // Empty ports when the runtime lets go of this port.
    virtual void setPorts(Ports ports) = 0;

    // A failure reply when a task with runs_on cannot be created now; empty when it can, and
    // for a local task. Creates nothing.
    [[nodiscard]] virtual QJsonObject admit(const BackgroundTaskIntent& intent,
                                            const QJsonObject& execution) const = 0;

    // Once the reserved task was approved: freezes its worktree and submits it, once. A submit
    // that ends before a record exists ends the task through Ports::failReserved.
    virtual void submit(const QString& taskId, const QJsonObject& execution) = 0;

    // nullopt when the task has no remote record; otherwise the code of the stop, empty when it
    // was taken or there was nothing to stop.
    [[nodiscard]] virtual std::optional<QString> stop(const QString& taskId) = 0;

    // `placement` and `runner` added to the task or tasks of a get or list reply.
    [[nodiscard]] virtual QJsonObject withPlacement(QJsonObject reply) const = 0;
    // Whether the task has a remote record.
    [[nodiscard]] virtual bool hasRecord(const QString& taskId) const = 0;
};

} // namespace ketplus::daemon::runner::entry
