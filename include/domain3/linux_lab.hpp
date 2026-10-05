#pragma once

#include "domain3/traffic_controller.hpp"
#include <csignal>
#include <iosfwd>
#include <string>

namespace domain3 {
struct LabConfig {
    Mode mode = Mode::shape;
    double rate_kbps = 256; // Decimal kilobits/s, IP headers included.
    std::size_t burst_bytes = 3000, queue_packets = 256;
    std::size_t packets = 100, payload_bytes = 1000;
    double send_pps = 500, timeout_seconds = 15;
    void validate() const;
};

struct LabResult {
    LabConfig config;
    TrafficStats left_to_right, right_to_left;
    std::size_t sent = 0, delivered = 0, echoed = 0, replies = 0;
    std::size_t duplicates = 0, malformed = 0, ignored_non_ipv4 = 0;
    double elapsed_seconds = 0, first_delivery_seconds = 0, last_delivery_seconds = 0;
    double average_rtt_ms = 0, max_rtt_ms = 0;
    bool timed_out = false, interrupted = false, namespace_restored = false;
};

extern volatile std::sig_atomic_t stop_requested;
LabResult run_lab(const LabConfig& config);
void print_result(const LabResult& result, std::ostream& out);
void write_json(const LabResult& result, const std::string& path);
int integration_tests(std::ostream& out);
} // namespace domain3
