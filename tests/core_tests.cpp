#include "domain3/token_bucket.hpp"
#include "domain3/traffic_controller.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool condition, const char* description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}
template <typename F> void rejects(F operation, const char* description) {
    bool rejected = false;
    try { operation(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, description);
}
}

int main() {
    try {
        using domain3::TokenBucket;
        TokenBucket bucket(1000, 1500, 0);
        check(bucket.consume(1500, 0), "initial burst allowed");
        check(!bucket.consume(1, 0), "empty bucket rejects traffic");
        check(std::abs(bucket.wait_seconds(500, 0) - 0.5) < 1e-12, "correct refill wait");
        check(bucket.consume(500, 0.5), "refill follows elapsed time");
        check(!bucket.consume(1, 0.5), "failed consumption did not add tokens");
        check(bucket.consume(1500, 100), "idle bucket caps at burst");
        check(!bucket.consume(1, 100), "idle time cannot accumulate unlimited credit");
        check(!bucket.can_fit(1501), "oversized packet detected");
        rejects([&] { bucket.wait_seconds(1501, 100); }, "oversized wait rejected");
        rejects([&] { bucket.consume(1, 99); }, "backwards clock rejected");
        rejects([] { TokenBucket b(0, 100, 0); }, "zero rate rejected");
        rejects([] { TokenBucket b(-1, 100, 0); }, "negative rate rejected");
        rejects([] { TokenBucket b(1, 0, 0); }, "zero burst rejected");
        rejects([] { TokenBucket b(std::numeric_limits<double>::infinity(), 1, 0); }, "infinite rate rejected");
        rejects([] { TokenBucket b(1, 1, std::numeric_limits<double>::quiet_NaN()); }, "NaN time rejected");

        // Check the defining traffic envelope: forwarded bytes <= burst + rate*t.
        TokenBucket envelope(12345, 2048, 0);
        std::size_t forwarded = 0;
        for (int tick = 0; tick < 10000; ++tick) {
            const double t = static_cast<double>(tick) / 1000.0;
            if (envelope.consume(1000, t)) forwarded += 1000;
            check(static_cast<double>(forwarded) <= 2048.0 + 12345.0 * t + 1e-7,
                  "token bucket envelope violated");
        }

        using domain3::Mode;
        using domain3::Packet;
        using domain3::TrafficController;
        std::vector<int> order;
        const TrafficController::Sink sink = [&](const Packet& p) { order.push_back(p[0]); return true; };
        TrafficController shape(Mode::shape, 100, 100, 2, 0);
        shape.receive(Packet(100, 1), 0, sink);
        shape.receive(Packet(100, 2), 0, sink);
        shape.receive(Packet(100, 3), 0, sink);
        shape.receive(Packet(100, 4), 0, sink);
        check(shape.stats().forwarded == 1 && shape.pending() == 2, "shaper queues excess traffic");
        check(shape.stats().queue_drops == 1 && shape.stats().queue_peak == 2, "bounded FIFO tail drop");
        shape.drain(0.5, sink);
        check(shape.stats().forwarded == 1, "partial refill cannot forward whole packet");
        shape.drain(1, sink);
        shape.drain(2, sink);
        check(order == std::vector<int>({1, 2, 3}), "FIFO ordering preserved");
        check(shape.pending() == 0 && shape.stats().max_queue_delay_seconds == 2, "queue drains with measured delay");

        TrafficController police(Mode::police, 100, 100, 2, 0);
        police.receive(Packet(100, 1), 0, sink);
        police.receive(Packet(100, 2), 0, sink);
        police.receive(Packet(100, 3), 1, sink);
        check(police.stats().forwarded == 2 && police.stats().rate_drops == 1, "policer drops excess but refills");
        check(police.pending() == 0 && police.stats().queue_peak == 0, "policer never queues");
        police.receive(Packet(101, 0), 1, sink);
        check(police.stats().oversized_drops == 1, "oversized packets cannot stall engine");

        TrafficController stopped(Mode::shape, 100, 100, 2, 0);
        stopped.receive(Packet(100, 1), 0, sink);
        stopped.receive(Packet(100, 2), 0, sink);
        stopped.discard_pending();
        check(stopped.stats().shutdown_drops == 1 && stopped.pending() == 0, "shutdown accounts for queued packets");
        TrafficController failed_io(Mode::police, 100, 100, 2, 0);
        failed_io.receive(Packet(100, 1), 0, [](const Packet&) { return false; });
        check(failed_io.stats().io_drops == 1 && failed_io.stats().forwarded == 0, "failed writes are not delivery");
        rejects([] { TrafficController t(Mode::shape, 100, 100, 0, 0); }, "zero queue rejected");

        // Independent direction controllers must not share token credit.
        TrafficController a(Mode::police, 100, 100, 2, 0), b(Mode::police, 100, 100, 2, 0);
        a.receive(Packet(100, 1), 0, sink);
        b.receive(Packet(100, 2), 0, sink);
        check(a.stats().forwarded == 1 && b.stats().forwarded == 1, "per-direction token isolation");
        std::cout << "PASS: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
