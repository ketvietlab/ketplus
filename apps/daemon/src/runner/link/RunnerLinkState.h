#pragma once

#include "runner/link/RunnerLinks.h"

#include <QList>
#include <QString>

#include <memory>

// One connection of RunnerLinks: its verified offer, its transport and its signaling counters.
// Internal to the links.
namespace ketplus::daemon::runner::link {

class RunnerLinks::Link final : public LinkTransportEvents {
    RunnerLinks& m_owner;

  public:
    enum class State {
        Answering, // offer taken, waiting for the channels
        Open,
    };
    struct RemoteCandidate {
        QString candidate; // empty: end of candidates
        QString mid;
    };

    Link(RunnerLinks& owner, VerifiedOffer offer) : m_owner(owner), offer(std::move(offer)) {}

    void localDescription(const QString& sdp) override {
        forward([&] { m_owner.localDescription(*this, sdp); });
    }
    void localCandidate(const QString& candidate, const QString& mid) override {
        forward([&] { m_owner.localCandidate(*this, candidate, mid); });
    }
    void opened() override {
        forward([&] { m_owner.opened(*this); });
    }
    void received(TransportChannel channel, const QByteArray& bytes, bool binary) override {
        forward([&] { m_owner.received(*this, channel, bytes, binary); });
    }
    void writable() override {
        forward([&] { m_owner.writable(*this); });
    }
    void closed() override {
        forward([&] { m_owner.transportClosed(*this); });
    }

    const VerifiedOffer offer;
    State state = State::Answering;
    std::unique_ptr<LinkTransport> transport;
    bool dropped = false;
    bool answerSent = false;
    bool applied = false; // the offer is applied; remote candidates go straight to the transport
    QList<RemoteCandidate> pendingCandidates;
    int remoteCandidates = 0;
    bool remoteDone = false;
    int localCandidates = 0;
    bool localDone = false;
    bool saidNotWritable = false;

  private:
    // Answers of a dropped transport are ignored; the link outlives the call that may drop it.
    template <typename Call> void forward(Call call) {
        if (dropped)
            return;
        const auto keep = m_owner.m_links.value(offer.connectionId);
        call();
    }
};

} // namespace ketplus::daemon::runner::link
