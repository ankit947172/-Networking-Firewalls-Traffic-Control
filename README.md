# Token-Bucket Network Bandwidth Shaper & Traffic Policer

**Domain 3: Networking, Firewalls & Traffic Control**  
**Linux · C++17 · TUN driver · network namespaces · real UDP traffic**

This capstone demonstrates how a network gateway controls bandwidth. A C++
application reads real IPv4 packets from a Linux TUN device, applies an
independent token bucket in each direction, and writes accepted packets to
another TUN device. Two isolated Linux network namespaces provide the endpoints.

- **Shaping:** queue packets until bandwidth credit becomes available.
- **Policing:** immediately drop packets that exceed available credit.
- **Bounded queues:** tail-drop excess packets to limit memory consumption.
- **Measurement:** count packets/bytes, classify drops, and measure queue delay and RTT.
- **Repeatable lab:** generate UDP probes, echo them, verify payloads, and clean up automatically.

This implements **one project from the Domain 3 list: the Token-Bucket Network
Bandwidth Shaper & Traffic Policer**. It does not implement the other five titles.
It is an educational traffic-control gateway, not a production firewall or VPN.

## Quick start

Use a Linux VM with TUN and network namespaces. Take a VM snapshot before the
demonstration. NAT is sufficient for installing dependencies; the experiment
itself needs no Internet connection or bridged adapter.

The project was built and tested in **Ubuntu on WSL2**, a virtualized Linux
environment, with Linux `6.6.87.2-microsoft-standard-WSL2` and GCC 15.2.0.
VirtualBox and VMware have not been independently tested. No VM image or
precompiled executable is included; rebuild the source inside your friend's VM.

From the extracted project directory in a Linux terminal:

```sh
sudo apt-get update
sudo apt-get install -y g++ cmake make
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 2
ctest --test-dir build --output-on-failure
sudo ./build/traffic_lab self-test
```

All implementation and automated test code is C++. CMake and the optional
GitHub workflow are configuration; terminal commands are execution instructions.
No Python or Java is required.

## Demonstrate the difference

```sh
# Queue excess traffic; the default queue can hold this experiment.
sudo ./build/traffic_lab lab --mode shape --rate-kbps 256 --packets 100

# Drop excess traffic immediately.
sudo ./build/traffic_lab lab --mode police --rate-kbps 64 --packets 100

# Show queue overflow / tail drop.
sudo ./build/traffic_lab lab --mode shape --rate-kbps 64 --queue-packets 2

# Export metrics; choose a NEW filename on every run.
sudo ./build/traffic_lab lab --mode shape --json shaping-result.json

./build/traffic_lab --help
```

The default sender offers 500 packets/s with 1,000-byte UDP payloads: about
**4,112 kbit/s**, including 20-byte IPv4 and 8-byte UDP headers. The configured
limit is in decimal kbit/s **per direction**. The bucket starts full, so short
runs can average above the steady-state rate due to the permitted burst.

For 100 probes at 256 kbit/s with a 3,000-byte burst, the theoretical minimum
forwarding duration is `(100 × 1028 − 3000) / 32000 = 3.119 seconds`.
Linux scheduling adds overhead. Intentional policing drops are a successful
demonstration, not a program failure.

## Architecture

```text
Namespace A                                Namespace B
UDP sender / echo receiver                  UDP echo endpoint
10.77.0.1:41000                             10.77.0.2:42000
        |                                         ^
    Linux IPv4                                Linux IPv4
        |                                         ^
    TUN tb-left -> read -> C++ policy -> write -> TUN tb-right
                           FIFO + bucket
        ^                                         |
        +--------- independent reverse path <-----+
```

One process owns all sockets, TUN descriptors, the generator, echo service, and
event loop. These are two isolated **network stacks**, not two VMs. Sockets
retain their creating namespaces after the process restores its original
namespace. The gateway transfers real IP packets through the Linux driver.

## Device-driver concepts

| Concept | Implementation |
| --- | --- |
| Character-device access | `open("/dev/net/tun")` |
| Driver control | `ioctl(TUNSETIFF)` creates a nonpersistent interface |
| Packet I/O | Nonblocking `read` / `write` exchange packets with the kernel |
| Readiness notification | `poll` waits for socket/device activity |
| Interface configuration | Address, netmask, MTU, and interface-flag ioctls |
| Resource lifetime | C++ RAII closes descriptors and releases temporary devices |
| Privilege and isolation | `unshare(CLONE_NEWNET)` and `setns` |

**This is C++ userspace software using an existing kernel driver, not a newly
written kernel module.** It implements relevant driver interaction. If the
trainer specifically requires authoring a custom driver, that is an additional
requirement not fulfilled here. Standard Linux kernel modules are normally
written in C; the requested implementation here is C++.

## Isolation and cleanup

- Configuration occurs only after creating and verifying a fresh network namespace.
- Neither endpoint connects to physical adapters or has a default route.
- No existing routes, firewall rules, DNS settings, forwarding sysctls, or services are changed.
- TUN interfaces are nonpersistent; no custom kernel module is compiled or loaded.
- There are no child processes or named namespaces to clean up.
- Normal exit, timeout, Ctrl+C, and process termination release owned descriptors.
  Final namespace disposal can complete asynchronously inside the kernel.
- JSON output refuses to overwrite files or follow a final symlink.

Existing Linux interfaces/routes matched before and after the integration test.
That is evidence for this environment, not a guarantee against every possible
OS or hypervisor fault.

## Repository layout

```text
CMakeLists.txt                         Linux-only C++17 build
include/domain3/token_bucket.hpp       Rate and burst algorithm
include/domain3/traffic_controller.hpp FIFO shaping and immediate policing
include/domain3/linux_lab.hpp          Lab configuration/result interface
src/linux_lab.cpp                     TUN, namespaces, UDP lab, JSON reports
src/main.cpp                          CLI and signal handling
tests/core_tests.cpp                  Deterministic policy tests
tests/integration_tests.cpp           Five real-packet test scenarios
docs/ARCHITECTURE.md                  Detailed design
docs/DEMO_AND_VIVA.md                 5–10 minute evaluation guide
docs/TESTING.md                       Verification and limitations
docs/VM_AND_SUBMISSION.md             Setup, troubleshooting, GitHub instructions
evidence/                            Recorded virtual Linux test results
.github/workflows/linux.yml           Optional Linux CI
```

Read [the evaluation guide](docs/DEMO_AND_VIVA.md) and
[the VM/submission guide](docs/VM_AND_SUBMISSION.md) before presenting.

## Limitations

- IPv4 UDP demonstration; no IPv6/TAP, TCP-specific validation, encryption,
  protocol inspection, or production routing integration.
- Basic IPv4 framing checks are not a security packet sanitizer.
- One bucket per direction, without per-flow fairness or priority classes.
- Bounded queues add latency and can overflow; policing deliberately loses traffic.
- Temporary nonblocking TUN write backpressure is counted as an I/O drop.
- The 1 ms polling timeout and ordinary Linux scheduling are not real-time guarantees.
- After sending finishes and queues empty, 300 ms of inactivity ends a lossy run.
  This is a lab settlement heuristic, not a WAN loss estimator.
- Extreme overload can cause kernel/socket loss outside the controller. Compare
  sent, received, forwarded, delivered, and reply counts to diagnose differences.

## References

- [Linux TUN/TAP documentation](https://docs.kernel.org/networking/tuntap.html)
- [Linux network namespaces](https://man7.org/linux/man-pages/man7/network_namespaces.7.html)
- [setns system call](https://man7.org/linux/man-pages/man2/setns.2.html)
- [Linux kernel implementation language](https://www.kernel.org/doc/html/next/process/programming-language.html)
