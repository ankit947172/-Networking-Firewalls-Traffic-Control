#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace domain3 {

// Time is supplied by the caller, allowing exact, sleep-free unit tests.
// The runtime uses std::chrono::steady_clock, never the wall clock.
class TokenBucket {
public:
    TokenBucket(double bytes_per_second, std::size_t burst_bytes, double now)
        : rate_(bytes_per_second), capacity_(static_cast<double>(burst_bytes)),
          tokens_(capacity_), last_(now) {
        if (!std::isfinite(rate_) || rate_ <= 0 || burst_bytes == 0 ||
            !std::isfinite(now)) {
            throw std::invalid_argument("rate, burst, and initial time must be valid");
        }
    }

    bool consume(std::size_t bytes, double now) {
        refill(now);
        const auto amount = static_cast<double>(bytes);
        if (amount > tokens_) return false;
        tokens_ -= amount;
        return true;
    }

    double wait_seconds(std::size_t bytes, double now) {
        refill(now);
        const auto amount = static_cast<double>(bytes);
        if (amount > capacity_) throw std::invalid_argument("packet exceeds burst capacity");
        return std::max(0.0, (amount - tokens_) / rate_);
    }

    bool can_fit(std::size_t bytes) const {
        return static_cast<double>(bytes) <= capacity_;
    }

private:
    void refill(double now) {
        if (!std::isfinite(now) || now < last_) {
            throw std::invalid_argument("token bucket requires monotonic finite time");
        }
        tokens_ = std::min(capacity_, tokens_ + (now - last_) * rate_);
        last_ = now;
    }
    double rate_;
    double capacity_;
    double tokens_;
    double last_;
};
} // namespace domain3
