#pragma once

#include "transport/PeerLink.h"
#include <QQueue>
#include <array>

namespace ketplus::daemon::transport {
class SendQueue final {
  public:
    static constexpr qsizetype eventsLimit = 4 * 1024 * 1024;
    static constexpr quint64 highWater = 256 * 1024;
    bool append(const QList<Packet>& packets) {
        auto size = sizes_;
        for (const auto& packet : packets) {
            const auto index = static_cast<size_t>(packet.channel);
            size[index] += packet.bytes.size();
            const qsizetype limit = index == 0   ? 256 * 1024
                                    : index == 1 ? eventsLimit
                                                 : 16 * 1024 * 1024;
            if (size[index] > limit)
                return false;
        }
        sizes_ = size;
        for (const auto& packet : packets)
            queues_[static_cast<size_t>(packet.channel)].enqueue(packet);
        return true;
    }
    void flush(PeerLink& link) {
        for (size_t i = 0; i < queues_.size(); ++i) {
            auto& queue = queues_[i];
            while (!queue.isEmpty()) {
                const auto& next = queue.head();
                if (link.buffered(static_cast<Channel>(i)) + quint64(next.bytes.size()) >
                        highWater ||
                    !link.send(next))
                    return;
                sizes_[i] -= next.bytes.size();
                queue.dequeue();
            }
        }
    }
    bool empty() const { return sizes_[0] + sizes_[1] + sizes_[2] == 0; }

  private:
    std::array<QQueue<Packet>, 3> queues_;
    std::array<qsizetype, 3> sizes_{};
};
} // namespace ketplus::daemon::transport
