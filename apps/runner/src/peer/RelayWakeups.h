#pragma once

#include <QJsonObject>
#include <QString>

// The wake-up messages the Relay sends a runner. They are hints only: the listener reads the
// HTTP routes before acting, so nothing here acquires a job or changes a pin.
namespace ketplus::runner::peer {

class WakeupListener {
  public:
    virtual ~WakeupListener() = default;
    virtual void jobAvailable(const QString& jobId) = 0;
    virtual void jobCancel(const QString& jobId, const QString& leaseId) = 0;
    virtual void runnerPaired(const QString& ticketId, const QString& pairState,
                              const QString& approvalSignature) = 0;
    virtual void runnerUnpaired(const QString& machineId) = 0;
};

enum class WakeupResult {
    Delivered,
    Malformed, // a wake-up type with a missing, empty or mistyped field: dropped
    NotWakeup, // another type, known or not
};

// Checks `message` against its type's fields and hands it to `listener`.
WakeupResult dispatchWakeup(const QJsonObject& message, WakeupListener& listener);

} // namespace ketplus::runner::peer
