#include "domain3/linux_lab.hpp"

#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void stop(int) { domain3::stop_requested = 1; }
void help() {
    std::cout <<
        "Domain 3: Token-Bucket Network Bandwidth Shaper & Traffic Policer\n"
        "C++17 / Linux / isolated TUN network lab\n\n"
        "Usage:\n"
        "  traffic_lab lab [options]       Run real UDP traffic through two TUN devices\n"
        "  traffic_lab self-test           Run privileged isolated integration tests\n"
        "  traffic_lab --help\n\n"
        "Options (lab only):\n"
        "  --mode shape|police       Queue excess traffic or drop it (default shape)\n"
        "  --rate-kbps N             Decimal kbit/s per direction (default 256)\n"
        "  --burst-bytes N           Initial/max credit, >=1500 (default 3000)\n"
        "  --queue-packets N         FIFO capacity per direction (default 256)\n"
        "  --packets N               UDP probes, 1..10000 (default 100)\n"
        "  --payload-bytes N         UDP payload, 8..1400 (default 1000)\n"
        "  --send-pps N              Offered packets/second (default 500)\n"
        "  --timeout N               Runtime ceiling in seconds (default 15)\n"
        "  --json PATH               Write results to a NEW file; never overwrite\n\n"
        "Run lab/self-test with sudo inside a Linux VM. Each run creates two\n"
        "anonymous network namespaces, with no external links or default routes.\n"
        "Rate includes IPv4 and UDP headers; an initial burst is permitted.\n"
        "Exit: 0 completed, 1 error/test failure, 2 timeout, 130 interrupted.\n";
}

std::size_t integer(const std::string& text) {
    std::size_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size())
        throw std::invalid_argument("expected a non-negative integer: " + text);
    return value;
}
double number(const std::string& text) {
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (errno != 0 || text.empty() || end != text.c_str() + text.size())
        throw std::invalid_argument("expected a number: " + text);
    return value;
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) { help(); return 0; }
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        const std::string command = argv[1];
        if (command == "self-test") {
            if (argc != 2) throw std::invalid_argument("self-test takes no options");
            return domain3::integration_tests(std::cout);
        }
        if (command != "lab") throw std::invalid_argument("unknown command; use --help");
        domain3::LabConfig config;
        std::string json_path;
        for (int i = 2; i < argc; i += 2) {
            if (i + 1 == argc) throw std::invalid_argument("missing value for " + std::string(argv[i]));
            const std::string key = argv[i], value = argv[i + 1];
            if (key == "--mode") {
                if (value == "shape") config.mode = domain3::Mode::shape;
                else if (value == "police") config.mode = domain3::Mode::police;
                else throw std::invalid_argument("mode must be shape or police");
            } else if (key == "--rate-kbps") config.rate_kbps = number(value);
            else if (key == "--burst-bytes") config.burst_bytes = integer(value);
            else if (key == "--queue-packets") config.queue_packets = integer(value);
            else if (key == "--packets") config.packets = integer(value);
            else if (key == "--payload-bytes") config.payload_bytes = integer(value);
            else if (key == "--send-pps") config.send_pps = number(value);
            else if (key == "--timeout") config.timeout_seconds = number(value);
            else if (key == "--json" && !value.empty()) json_path = value;
            else throw std::invalid_argument("unknown or invalid option: " + key);
        }
        const auto result = domain3::run_lab(config);
        domain3::print_result(result, std::cout);
        if (!json_path.empty()) domain3::write_json(result, json_path);
        if (result.interrupted) return 130;
        return result.timed_out ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
