#include "domain3/linux_lab.hpp"

#include <iostream>
#include <stdexcept>

namespace domain3 {
int integration_tests(std::ostream& out) {
    std::size_t checks = 0;
    const auto require = [&](bool condition, const char* message) {
        ++checks;
        if (!condition) throw std::runtime_error(message);
    };
    const auto common = [&](const LabResult& r) {
        require(r.namespace_restored, "original network namespace not restored");
        require(!r.interrupted, "test interrupted");
        require(r.malformed == 0 && r.duplicates == 0, "payload corruption or duplication");
        for (const auto* s : {&r.left_to_right, &r.right_to_left}) {
            require(s->received == s->forwarded + s->drops(), "packet conservation failed");
            require(s->io_drops == 0 && s->oversized_drops == 0, "unexpected packet drop");
            const double envelope = static_cast<double>(r.config.burst_bytes) +
                                    r.config.rate_kbps * 125.0 * r.elapsed_seconds;
            require(static_cast<double>(s->forwarded_bytes) <= envelope + 1.0, "bandwidth envelope exceeded");
            require(s->queue_peak <= r.config.queue_packets, "queue exceeded capacity");
        }
    };
    LabConfig config;
    config.packets = 40;
    config.rate_kbps = 256;
    config.timeout_seconds = 8;

    out << "[1/5] Shaping: intact bidirectional delivery, queueing, and rate envelope\n";
    auto r = run_lab(config);
    print_result(r, out);
    common(r);
    require(!r.timed_out && r.sent == config.packets && r.delivered == config.packets &&
            r.replies == config.packets, "shaping failed to deliver all probes and echoes");
    require(r.left_to_right.queue_peak > 0 && r.left_to_right.drops() == 0 &&
            r.right_to_left.drops() == 0, "shaping should queue without dropping");
    const double min_duration = (static_cast<double>(config.packets * (config.payload_bytes + 28)) -
                                 static_cast<double>(config.burst_bytes)) / (config.rate_kbps * 125.0);
    require(r.last_delivery_seconds + 0.002 >= min_duration, "shaper delivered too fast");

    out << "[2/5] Policing: excess traffic dropped, no queue\n";
    config.mode = Mode::police;
    config.rate_kbps = 64;
    r = run_lab(config);
    print_result(r, out);
    common(r);
    require(!r.timed_out && r.delivered > 0 && r.delivered < config.packets, "policer delivery mismatch");
    require(r.left_to_right.rate_drops > 0 && r.left_to_right.queue_peak == 0,
            "policer should drop without queueing");
    require(r.left_to_right.received == config.packets, "policer did not observe every offered probe");

    out << "[3/5] Bounded queue: tail drop and eventual drain\n";
    config.mode = Mode::shape;
    config.queue_packets = 2;
    r = run_lab(config);
    print_result(r, out);
    common(r);
    require(!r.timed_out && r.left_to_right.queue_drops > 0 && r.left_to_right.queue_peak == 2,
            "bounded queue did not overflow as expected");
    require(r.replies == r.delivered && r.delivered > 0, "queued traffic did not drain end to end");

    out << "[4/5] Deadline: queued traffic accounted as shutdown drops\n";
    config.queue_packets = 256;
    config.rate_kbps = 8;
    config.timeout_seconds = 0.2;
    r = run_lab(config);
    print_result(r, out);
    common(r);
    require(r.timed_out && r.left_to_right.shutdown_drops > 0, "deadline did not discard pending queue");

    out << "[5/5] New run after teardown: low offered load passes unchanged\n";
    config = LabConfig {};
    config.packets = 10;
    config.send_pps = 20;
    config.mode = Mode::police;
    r = run_lab(config);
    print_result(r, out);
    common(r);
    require(!r.timed_out && r.replies == config.packets && r.left_to_right.drops() == 0 &&
            r.right_to_left.drops() == 0, "low-load delivery failed after earlier namespace teardown");
    out << "PASS: 5 isolated real-packet scenarios, " << checks << " integration checks\n";
    return 0;
}
} // namespace domain3
