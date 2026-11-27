#pragma once

#include "delivery/JobDelivery.h"
#include "session/SessionPorts.h"

// The AcceptedJobs of a session that JobDelivery feeds: a record that cannot be written stops
// the delivery from taking jobs until the process restarts. The session refuses that spec.
namespace ketplus::runner::delivery {

class GuardedAcceptedJobs final : public session::AcceptedJobs {
  public:
    GuardedAcceptedJobs(session::AcceptedJobs& record, JobDelivery& delivery)
        : m_record(record), m_delivery(delivery) {}

    [[nodiscard]] bool contains(const QString& jobId, const qint64 attempt) const override {
        return m_record.contains(jobId, attempt);
    }
    [[nodiscard]] bool add(const QString& jobId, const qint64 attempt) override {
        if (m_record.add(jobId, attempt))
            return true;
        m_delivery.storageFailed(QStringLiteral("The record of accepted jobs could not be "
                                                "written. This runner takes no new job until "
                                                "it is restarted."));
        return false;
    }

  private:
    session::AcceptedJobs& m_record;
    JobDelivery& m_delivery;
};

} // namespace ketplus::runner::delivery
