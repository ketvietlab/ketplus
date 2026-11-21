#include "run/ExecutorCommandHost.h"

namespace ketplus::runner::app {

ExecutorCommandHost::ExecutorCommandHost(exec::ExecutorCommandRunner::Options options,
                                         exec::Executor& executor, exec::PlatformPolicy& policy) {
    exec::ExecutorCommandRunner::Events events;
    events.output = [this](const QByteArray& bytes) {
        if (m_events.output)
            m_events.output(bytes);
    };
    events.outputLost = [this](const qint64 bytes) {
        if (m_events.outputLost)
            m_events.outputLost(bytes);
    };
    events.finished = [this](const session::CommandExit& exit) {
        if (m_events.finished)
            m_events.finished(exit);
    };
    events.readinessChanged = [this](const exec::Readiness&) {
        if (m_changed)
            m_changed();
    };
    m_runner = std::make_unique<exec::ExecutorCommandRunner>(std::move(options), executor, policy,
                                                             std::move(events));
}

ExecutorCommandHost::~ExecutorCommandHost() = default;

CommandHost::Readiness ExecutorCommandHost::readiness() const {
    switch (m_runner->readiness().state) {
    case exec::Readiness::State::Ready:
        return Readiness::Ready;
    case exec::Readiness::State::Unavailable:
        return Readiness::Unavailable;
    case exec::Readiness::State::Unknown:
        break;
    }
    return Readiness::Unknown;
}

void ExecutorCommandHost::cleanUpLeftovers(std::function<void(bool)> done) {
    m_runner->probe();
    m_runner->cleanUpLeftovers(
        [done = std::move(done)](const bool clean, const QString&) { done(clean); });
}

} // namespace ketplus::runner::app
