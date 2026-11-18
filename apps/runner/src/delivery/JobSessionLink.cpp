#include "delivery/JobSessionLink.h"

namespace ketplus::runner::delivery {

using Phase = session::JobSession::Phase;

void JobSessionLink::open(const HeldLease& lease) {
    (void)m_session->assign({lease.jobId, lease.attempt, lease.machineId});
}

QString JobSessionLink::phase() const {
    switch (m_session->phase()) {
    case Phase::Idle:
        return {};
    case Phase::Assigned:
    case Phase::Starting:
        return QStringLiteral("assigned");
    case Phase::Syncing:
        return QStringLiteral("syncing");
    case Phase::Running:
    case Phase::Stopping:
    case Phase::Delivering:
        return QStringLiteral("running");
    }
    return {};
}

} // namespace ketplus::runner::delivery
