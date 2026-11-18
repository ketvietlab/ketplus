#include "run/RelayQueue.h"

#include <QMetaObject>

#include <algorithm>

namespace ketplus::runner::app {

RelayQueue::RelayQueue(RelayPort& port, RelayQueueListener& listener)
    : m_port(port), m_listener(listener) {}

bool RelayQueue::heartbeatWaiting() const {
    return std::any_of(m_items.cbegin(), m_items.cend(),
                       [](const Item& item) { return item.kind == Item::Kind::Heartbeat; });
}

void RelayQueue::heartbeat() {
    if (!heartbeatWaiting())
        enqueue({Item::Kind::Heartbeat, {}, false, {}});
}

void RelayQueue::pairings() {
    const bool waiting = std::any_of(m_items.cbegin(), m_items.cend(), [](const Item& item) {
        return item.kind == Item::Kind::Pairings;
    });
    if (!waiting)
        enqueue({Item::Kind::Pairings, {}, false, {}});
}

void RelayQueue::send(const delivery::LeaseCall& call) {
    enqueue({Item::Kind::Lease, call, false, {}});
}

void RelayQueue::current(Answer answer) {
    enqueue({Item::Kind::Token, {}, false, std::move(answer)});
}

void RelayQueue::renew(Answer answer) { enqueue({Item::Kind::Token, {}, true, std::move(answer)}); }

void RelayQueue::close() {
    m_closed = true;
    m_items.clear();
}

void RelayQueue::enqueue(Item item) {
    if (m_closed)
        return;
    m_items.append(std::move(item));
    schedule();
}

// The pump always runs from the event loop, so an answer never reaches a piece from inside its
// own send.
void RelayQueue::schedule() {
    if (m_scheduled || m_closed)
        return;
    m_scheduled = true;
    QMetaObject::invokeMethod(
        &m_home,
        [this] {
            m_scheduled = false;
            pump();
        },
        Qt::QueuedConnection);
}

// A pump that comes while a call runs (from its nested event loop) does nothing: the running
// call schedules the next one when it is done.
void RelayQueue::pump() {
    if (m_running || m_closed || m_items.isEmpty())
        return;
    const Item item = m_items.takeFirst();
    run(item);
    if (!m_items.isEmpty())
        schedule();
}

void RelayQueue::run(const Item& item) {
    m_running = true;
    switch (item.kind) {
    case Item::Kind::Heartbeat: {
        const AccessTokens::Call call = m_port.heartbeat(m_listener.heartbeatBody());
        m_running = false;
        if (!m_closed)
            m_listener.heartbeatAnswered(call);
        return;
    }
    case Item::Kind::Pairings: {
        const std::optional<ExitCode> exit = m_port.pairings();
        m_running = false;
        if (!m_closed)
            m_listener.pairingsChecked(exit);
        return;
    }
    case Item::Kind::Lease: {
        const delivery::CallResult result = m_port.lease(item.call);
        m_running = false;
        if (!m_closed)
            m_listener.leaseAnswered(item.call, result);
        return;
    }
    case Item::Kind::Token: {
        const RelayPort::Token token = m_port.token(item.renew);
        m_running = false;
        if (m_closed)
            return;
        Token answer;
        switch (token.outcome) {
        case AccessTokens::Outcome::Ok:
            answer = {Outcome::Ok, token.value};
            break;
        case AccessTokens::Outcome::Retry:
            answer.outcome = Outcome::Retry;
            break;
        case AccessTokens::Outcome::InvalidAccess:
            answer.outcome = Outcome::InvalidAccess;
            break;
        case AccessTokens::Outcome::RunnerRemoved:
            answer.outcome = Outcome::RunnerRemoved;
            break;
        case AccessTokens::Outcome::StateFailure:
            m_listener.tokenFailed(token.outcome);
            answer.outcome = Outcome::Retry;
            break;
        }
        if (!m_closed && item.answer)
            item.answer(answer);
        return;
    }
    }
    m_running = false;
}

} // namespace ketplus::runner::app
