#include "domain3/linux_lab.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace domain3 {
volatile std::sig_atomic_t stop_requested = 0;
namespace {
using Clock = std::chrono::steady_clock;
double seconds() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
[[noreturn]] void fail(const char* action) {
    throw std::system_error(errno, std::generic_category(), action);
}
class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) ::close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    int get() const { return fd_; }
private:
    int fd_;
};

Fd current_namespace() {
    Fd fd(::open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0) fail("open network namespace");
    return fd;
}

bool same_namespace(int a, int b) {
    struct stat sa {}, sb {};
    if (::fstat(a, &sa) < 0 || ::fstat(b, &sb) < 0) fail("stat namespace");
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

class NamespaceRestore {
public:
    explicit NamespaceRestore(int original) : original_(original) {}
    ~NamespaceRestore() {
        if (!restored_ && ::setns(original_, CLONE_NEWNET) < 0)
            std::cerr << "Could not restore this process's namespace; process will exit.\n";
    }
    void restore() {
        if (::setns(original_, CLONE_NEWNET) < 0) fail("restore original network namespace");
        restored_ = true;
    }
private:
    int original_;
    bool restored_ = false;
};

sockaddr_in address(const char* ip, std::uint16_t port) {
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip, &addr.sin_addr) != 1)
        throw std::invalid_argument("invalid internal IPv4 address");
    return addr;
}

ifreq interface_request(const char* name) {
    ifreq req {};
    if (std::strlen(name) >= IFNAMSIZ) throw std::invalid_argument("interface name too long");
    std::memcpy(req.ifr_name, name, std::strlen(name) + 1);
    return req;
}

void interface_address(int control, const char* name, const char* ip, unsigned long operation) {
    auto req = interface_request(name);
    const auto addr = address(ip, 0);
    static_assert(sizeof(addr) == sizeof(req.ifr_addr));
    std::memcpy(&req.ifr_addr, &addr, sizeof(addr));
    if (::ioctl(control, operation, &req) < 0) fail("configure isolated interface address");
}

void interface_up(int control, const char* name) {
    auto req = interface_request(name);
    if (::ioctl(control, SIOCGIFFLAGS, &req) < 0) fail("read interface flags");
    req.ifr_flags = static_cast<short>(req.ifr_flags | IFF_UP);
    if (::ioctl(control, SIOCSIFFLAGS, &req) < 0) fail("bring isolated interface up");
}

struct Endpoint { Fd netns, tun, socket; };

Endpoint create_endpoint(int original, const char* name, const char* ip, std::uint16_t port) {
    // Crucial ordering: no interface creation/configuration until unshare succeeds.
    if (::unshare(CLONE_NEWNET) < 0) fail("unshare network namespace (run lab with sudo)");
    NamespaceRestore restore(original);
    Endpoint endpoint;
    endpoint.netns = current_namespace();
    if (same_namespace(original, endpoint.netns.get()))
        throw std::runtime_error("refusing to configure the original network namespace");

    endpoint.tun = Fd(::open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC));
    if (endpoint.tun.get() < 0) fail("open /dev/net/tun");
    auto req = interface_request(name);
    req.ifr_flags = static_cast<short>(IFF_TUN | IFF_NO_PI | IFF_TUN_EXCL);
    if (::ioctl(endpoint.tun.get(), TUNSETIFF, &req) < 0) fail("TUNSETIFF");
    // No TUNSETPERSIST: closing the descriptor removes the virtual device.
    Fd control(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (control.get() < 0) fail("interface control socket");
    interface_address(control.get(), name, ip, SIOCSIFADDR);
    interface_address(control.get(), name, "255.255.255.252", SIOCSIFNETMASK);
    auto mtu = interface_request(name);
    mtu.ifr_mtu = 1500;
    if (::ioctl(control.get(), SIOCSIFMTU, &mtu) < 0) fail("set isolated TUN MTU");
    interface_up(control.get(), name);
    interface_up(control.get(), "lo");

    endpoint.socket = Fd(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (endpoint.socket.get() < 0) fail("UDP socket");
    const int buffer_size = 4 * 1024 * 1024;
    if (::setsockopt(endpoint.socket.get(), SOL_SOCKET, SO_RCVBUF,
                     &buffer_size, sizeof(buffer_size)) < 0) fail("UDP receive buffer");
    const auto local = address(ip, port);
    if (::bind(endpoint.socket.get(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0)
        fail("bind isolated UDP endpoint");
    restore.restore();
    return endpoint;
}

bool temporary_error() { return errno == EAGAIN || errno == EWOULDBLOCK; }

bool inject(int fd, const Packet& packet) {
    ssize_t n;
    do { n = ::write(fd, packet.data(), packet.size()); } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (temporary_error() || errno == ENOBUFS) return false;
        fail("write TUN packet");
    }
    return static_cast<std::size_t>(n) == packet.size();
}

void read_packets(int fd, TrafficController& controller, const TrafficController::Sink& sink,
                  LabResult& result) {
    std::array<std::uint8_t, 65536> buffer {};
    for (int batch = 0; batch < 256; ++batch) {
        const auto n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            if (temporary_error()) break;
            fail("read TUN packet");
        }
        if (n == 0) throw std::runtime_error("unexpected TUN EOF");
        const auto size = static_cast<std::size_t>(n);
        if ((buffer[0] >> 4) != 4) { ++result.ignored_non_ipv4; continue; }
        const auto header_size = static_cast<std::size_t>(buffer[0] & 15U) * 4U;
        const auto total_size = size >= 4 ? (static_cast<std::size_t>(buffer[2]) << 8U) | buffer[3] : 0;
        if (size < 20 || header_size < 20 || header_size > size || total_size != size) {
            ++result.malformed;
            continue;
        }
        controller.receive(Packet(buffer.begin(), buffer.begin() + n), seconds(), sink);
    }
}

Packet probe(std::size_t payload_bytes, std::size_t sequence) {
    Packet packet(payload_bytes, 0x5a);
    packet[0] = 'D'; packet[1] = '3'; packet[2] = 'T'; packet[3] = 'B';
    const auto seq = htonl(static_cast<std::uint32_t>(sequence));
    std::memcpy(packet.data() + 4, &seq, sizeof(seq));
    return packet;
}

bool decode_probe(const std::uint8_t* data, std::size_t size, const LabConfig& config,
                  std::size_t& sequence) {
    if (size != config.payload_bytes || size < 8 || data[0] != 'D' || data[1] != '3' ||
        data[2] != 'T' || data[3] != 'B') return false;
    std::uint32_t seq;
    std::memcpy(&seq, data + 4, sizeof(seq));
    sequence = ntohl(seq);
    if (sequence >= config.packets) return false;
    for (std::size_t i = 8; i < size; ++i) if (data[i] != 0x5a) return false;
    return true;
}

bool send_datagram(int fd, const std::uint8_t* data, std::size_t size, const sockaddr_in& peer) {
    ssize_t n;
    do {
        n = ::sendto(fd, data, size, 0, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (temporary_error() || errno == ENOBUFS) return false;
        fail("send UDP probe");
    }
    if (static_cast<std::size_t>(n) != size) throw std::runtime_error("partial UDP send");
    return true;
}
} // namespace

void LabConfig::validate() const {
    if (!std::isfinite(rate_kbps) || rate_kbps < 1 || rate_kbps > 1000000)
        throw std::invalid_argument("rate-kbps must be between 1 and 1000000");
    if (burst_bytes < 1500 || burst_bytes > 16 * 1024 * 1024)
        throw std::invalid_argument("burst-bytes must be between 1500 and 16777216 (at least MTU)");
    if (queue_packets < 1 || queue_packets > 8192)
        throw std::invalid_argument("queue-packets must be between 1 and 8192");
    if (packets < 1 || packets > 10000) throw std::invalid_argument("packets must be between 1 and 10000");
    if (payload_bytes < 8 || payload_bytes > 1400)
        throw std::invalid_argument("payload-bytes must be between 8 and 1400 (no fragmentation)");
    if (!std::isfinite(send_pps) || send_pps < 1 || send_pps > 10000)
        throw std::invalid_argument("send-pps must be between 1 and 10000");
    if (!std::isfinite(timeout_seconds) || timeout_seconds < 0.1 || timeout_seconds > 120)
        throw std::invalid_argument("timeout must be between 0.1 and 120 seconds");
}

LabResult run_lab(const LabConfig& config) {
    config.validate();
    LabResult result;
    result.config = config;
    const auto original = current_namespace();
    auto left = create_endpoint(original.get(), "tb-left", "10.77.0.1", 41000);
    auto right = create_endpoint(original.get(), "tb-right", "10.77.0.2", 42000);
    const auto restored = current_namespace();
    result.namespace_restored = same_namespace(original.get(), restored.get());
    if (!result.namespace_restored) throw std::runtime_error("namespace restoration verification failed");
    // Sockets and TUN descriptors retain their namespaces after setns restores us.
    const auto left_peer = address("10.77.0.1", 41000);
    const auto right_peer = address("10.77.0.2", 42000);
    const double start = seconds();
    const double rate_bytes = config.rate_kbps * 1000.0 / 8.0;
    TrafficController forward(config.mode, rate_bytes, config.burst_bytes, config.queue_packets, start);
    TrafficController reverse(config.mode, rate_bytes, config.burst_bytes, config.queue_packets, start);
    const TrafficController::Sink to_right = [&](const Packet& p) { return inject(right.tun.get(), p); };
    const TrafficController::Sink to_left = [&](const Packet& p) { return inject(left.tun.get(), p); };
    std::vector<double> sent_times(config.packets, 0);
    std::vector<bool> delivered(config.packets, false), replied(config.packets, false);
    double rtt_sum = 0, last_activity = start;
    std::array<std::uint8_t, 2048> buffer {};

    while (!stop_requested) {
        double now = seconds();
        if (now - start >= config.timeout_seconds) { result.timed_out = true; break; }
        // Bounded batches prevent the generator from starving the gateway.
        for (int batch = 0; batch < 64 && result.sent < config.packets; ++batch) {
            if (now - start < static_cast<double>(result.sent) / config.send_pps) break;
            const auto packet = probe(config.payload_bytes, result.sent);
            if (!send_datagram(left.socket.get(), packet.data(), packet.size(), right_peer)) break;
            sent_times[result.sent++] = now;
            last_activity = now;
            now = seconds();
        }
        const auto before = forward.stats().received + reverse.stats().received +
                            forward.stats().forwarded + reverse.stats().forwarded;
        read_packets(left.tun.get(), forward, to_right, result);
        forward.drain(seconds(), to_right);
        read_packets(right.tun.get(), reverse, to_left, result);
        reverse.drain(seconds(), to_left);
        if (before != forward.stats().received + reverse.stats().received +
                      forward.stats().forwarded + reverse.stats().forwarded) last_activity = seconds();

        for (int side = 0; side < 2; ++side) {
            const int fd = side == 0 ? right.socket.get() : left.socket.get();
            for (int batch = 0; batch < 256; ++batch) {
                const auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    if (temporary_error()) break;
                    fail("receive UDP probe");
                }
                last_activity = seconds();
                std::size_t sequence = 0;
                if (!decode_probe(buffer.data(), static_cast<std::size_t>(n), config, sequence) ||
                    sequence >= result.sent) { ++result.malformed; continue; }
                auto& seen = side == 0 ? delivered : replied;
                if (seen[sequence]) { ++result.duplicates; continue; }
                seen[sequence] = true;
                if (side == 0) {
                    if (result.delivered++ == 0) result.first_delivery_seconds = last_activity - start;
                    result.last_delivery_seconds = last_activity - start;
                    if (send_datagram(fd, buffer.data(), static_cast<std::size_t>(n), left_peer)) ++result.echoed;
                } else {
                    ++result.replies;
                    const double rtt = (last_activity - sent_times[sequence]) * 1000.0;
                    rtt_sum += rtt;
                    result.max_rtt_ms = std::max(result.max_rtt_ms, rtt);
                }
            }
        }
        if (result.replies == config.packets) break;
        if (result.sent == config.packets && forward.pending() == 0 && reverse.pending() == 0 &&
            seconds() - last_activity >= 0.3) break;
        pollfd fds[] = {{left.tun.get(), POLLIN, 0}, {right.tun.get(), POLLIN, 0},
                        {left.socket.get(), POLLIN, 0}, {right.socket.get(), POLLIN, 0}};
        if (::poll(fds, 4, 1) < 0 && errno != EINTR) fail("poll lab descriptors");
        for (const auto& fd : fds)
            if ((fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                throw std::runtime_error("lab descriptor reported an I/O error");
    }
    result.interrupted = stop_requested != 0;
    result.elapsed_seconds = seconds() - start;
    result.average_rtt_ms = result.replies ? rtt_sum / static_cast<double>(result.replies) : 0;
    forward.discard_pending();
    reverse.discard_pending();
    result.left_to_right = forward.stats();
    result.right_to_left = reverse.stats();
    return result; // RAII closes sockets, TUN devices, and anonymous namespace handles.
}

void print_result(const LabResult& r, std::ostream& out) {
    out << std::fixed << std::setprecision(3)
        << "Mode: " << (r.config.mode == Mode::shape ? "shape" : "police")
        << " | limit: " << r.config.rate_kbps << " kbit/s PER DIRECTION"
        << " | burst: " << r.config.burst_bytes << " bytes\n"
        << "UDP probes sent: " << r.sent << " | delivered: " << r.delivered
        << " | echo replies: " << r.replies << '\n';
    const auto direction = [&](const char* name, const TrafficStats& s) {
        out << name << ": received=" << s.received << " forwarded=" << s.forwarded
            << " IP-bytes=" << s.forwarded_bytes << " drops=" << s.drops()
            << " [rate=" << s.rate_drops << " queue=" << s.queue_drops
            << " oversized=" << s.oversized_drops << " io=" << s.io_drops
            << " shutdown=" << s.shutdown_drops << "]\n"
            << "  queue peak=" << s.queue_peak << " packets, max queue delay="
            << s.max_queue_delay_seconds * 1000.0 << " ms\n";
    };
    direction("Left -> right", r.left_to_right);
    direction("Right -> left", r.right_to_left);
    out << "Elapsed: " << r.elapsed_seconds << " s | average RTT: " << r.average_rtt_ms
        << " ms | max RTT: " << r.max_rtt_ms << " ms\n"
        << "Namespace restored: " << (r.namespace_restored ? "yes" : "NO")
        << " | timeout: " << (r.timed_out ? "yes" : "no")
        << " | interrupted: " << (r.interrupted ? "yes" : "no") << '\n';
}

void write_json(const LabResult& r, const std::string& path) {
    // O_EXCL + O_NOFOLLOW avoids clobbering an existing file while running as root.
    Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644));
    if (fd.get() < 0) fail("create report (choose a new filename)");
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    const auto direction = [&](const char* name, const TrafficStats& s) {
        out << "  \"" << name << "\": {\"received\": " << s.received
            << ", \"forwarded\": " << s.forwarded << ", \"forwarded_bytes\": " << s.forwarded_bytes
            << ", \"rate_drops\": " << s.rate_drops << ", \"queue_drops\": " << s.queue_drops
            << ", \"oversized_drops\": " << s.oversized_drops << ", \"io_drops\": " << s.io_drops
            << ", \"shutdown_drops\": " << s.shutdown_drops << ", \"queue_peak\": " << s.queue_peak
            << ", \"max_queue_delay_ms\": " << s.max_queue_delay_seconds * 1000.0 << "}";
    };
    out << "{\n  \"mode\": \"" << (r.config.mode == Mode::shape ? "shape" : "police") << "\",\n"
        << "  \"rate_kbps_per_direction\": " << r.config.rate_kbps << ",\n"
        << "  \"burst_bytes\": " << r.config.burst_bytes << ",\n"
        << "  \"queue_limit_packets\": " << r.config.queue_packets << ",\n"
        << "  \"requested_packets\": " << r.config.packets << ",\n"
        << "  \"payload_bytes\": " << r.config.payload_bytes << ",\n"
        << "  \"send_pps\": " << r.config.send_pps << ",\n"
        << "  \"timeout_seconds\": " << r.config.timeout_seconds << ",\n"
        << "  \"sent\": " << r.sent << ", \"delivered\": " << r.delivered
        << ", \"echoed\": " << r.echoed << ", \"replies\": " << r.replies << ",\n"
        << "  \"duplicates\": " << r.duplicates << ", \"malformed\": " << r.malformed << ",\n"
        << "  \"ignored_non_ipv4\": " << r.ignored_non_ipv4 << ",\n"
        << "  \"elapsed_seconds\": " << r.elapsed_seconds << ",\n"
        << "  \"first_delivery_seconds\": " << r.first_delivery_seconds << ",\n"
        << "  \"last_delivery_seconds\": " << r.last_delivery_seconds << ",\n"
        << "  \"average_rtt_ms\": " << r.average_rtt_ms << ", \"max_rtt_ms\": " << r.max_rtt_ms << ",\n"
        << "  \"timed_out\": " << (r.timed_out ? "true" : "false") << ",\n"
        << "  \"interrupted\": " << (r.interrupted ? "true" : "false") << ",\n"
        << "  \"namespace_restored\": " << (r.namespace_restored ? "true" : "false") << ",\n";
    direction("left_to_right", r.left_to_right); out << ",\n";
    direction("right_to_left", r.right_to_left); out << "\n}\n";
    const auto text = out.str();
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto n = ::write(fd.get(), text.data() + offset, text.size() - offset);
        if (n < 0) { if (errno == EINTR) continue; fail("write report"); }
        if (n == 0) throw std::runtime_error("zero-length report write");
        offset += static_cast<std::size_t>(n);
    }
}
} // namespace domain3
