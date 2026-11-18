#pragma once

#include "delivery/DeliveryPorts.h"
#include "session/JobSession.h"

// The SessionLink of a JobSession. The session is attached after it is made, since it takes
// JobDelivery::lease() as its lease.
namespace ketplus::runner::delivery {

class JobSessionLink final : public SessionLink {
  public:
    void attach(session::JobSession& session) { m_session = &session; }

    [[nodiscard]] bool open(const HeldLease& lease) override;
    [[nodiscard]] bool withdraw() override { return m_session->withdraw(); }
    void leaseStarted(const QString& startedAt) override { m_session->leaseStarted(startedAt); }
    void leaseStartRefused(const QString& code) override { m_session->leaseStartRefused(code); }
    void leaseLost(const QString& code, const QString& reason) override {
        m_session->leaseLost(code, reason);
    }
    [[nodiscard]] QString phase() const override;

  private:
    session::JobSession* m_session = nullptr;
};

} // namespace ketplus::runner::delivery
