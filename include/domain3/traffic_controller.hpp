#pragma once

#include "domain3/token_bucket.hpp"
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace domain3 {
using Packet = std::vector<std::uint8_t>;
enum class Mode { shape, police };

struct TrafficStats {
    std::uint64_t received = 0, forwarded = 0, forwarded_bytes = 0;
    std::uint64_t rate_drops = 0, queue_drops = 0, oversized_drops = 0;
    std::uint64_t io_drops = 0, shutdown_drops = 0;
    std::size_t queue_peak = 0;
    double max_queue_delay_seconds = 0;
    std::uint64_t drops() const {
        return rate_drops + queue_drops + oversized_drops + io_drops + shutdown_drops;
    }
};

// One controller per direction: independent bandwidth and bounded FIFO storage.
class TrafficController {
public:
    using Sink = std::function<bool(const Packet&)>;
    TrafficController(Mode mode, double bytes_per_second, std::size_t burst,
                      std::size_t queue_limit, double now)
        : mode_(mode), bucket_(bytes_per_second, burst, now), limit_(queue_limit) {
        if (queue_limit == 0) throw std::invalid_argument("queue limit must be positive");
    }

    void receive(Packet packet, double now, const Sink& sink) {
        ++stats_.received;
        if (!bucket_.can_fit(packet.size())) { ++stats_.oversized_drops; return; }
        if (mode_ == Mode::police) {
            if (bucket_.consume(packet.size(), now)) forward(packet, 0, sink);
            else ++stats_.rate_drops;
            return;
        }
        // Drain older packets first, preserving FIFO ordering.
        drain(now, sink);
        if (queue_.empty() && bucket_.consume(packet.size(), now)) {
            forward(packet, 0, sink);
        } else if (queue_.size() == limit_) {
            ++stats_.queue_drops; // Tail drop: retain packets already queued.
        } else {
            queue_.push_back({std::move(packet), now});
            stats_.queue_peak = std::max(stats_.queue_peak, queue_.size());
        }
    }

    void drain(double now, const Sink& sink) {
        while (!queue_.empty() && bucket_.consume(queue_.front().packet.size(), now)) {
            forward(queue_.front().packet, now - queue_.front().arrival, sink);
            queue_.pop_front();
        }
    }

    void discard_pending() {
        stats_.shutdown_drops += queue_.size();
        queue_.clear();
    }
    std::size_t pending() const { return queue_.size(); }
    const TrafficStats& stats() const { return stats_; }

private:
    struct QueuedPacket { Packet packet; double arrival; };
    void forward(const Packet& packet, double delay, const Sink& sink) {
        // Failed nonblocking writes are explicit I/O drops. Tokens stay spent.
        if (!sink(packet)) { ++stats_.io_drops; return; }
        ++stats_.forwarded;
        stats_.forwarded_bytes += packet.size();
        stats_.max_queue_delay_seconds = std::max(stats_.max_queue_delay_seconds, delay);
    }
    Mode mode_;
    TokenBucket bucket_;
    std::size_t limit_;
    std::deque<QueuedPacket> queue_;
    TrafficStats stats_;
};
} // namespace domain3
