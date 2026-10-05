# Verification report

Test date: **30 September 2026**. Environment: **Ubuntu 26.04 LTS under WSL2**,
Linux `6.6.87.2-microsoft-standard-WSL2`, GCC 15.2.0, CMake 4.2.3.
This is virtualized Linux, not a separately tested VirtualBox/VMware guest.

## Results

| Check | Observed result |
| --- | --- |
| C++ Debug build, ASan + UBSan | Compiled; tests completed without sanitizer diagnostics |
| C++ Release build | Compiled and passed CTest |
| Deterministic policy checks | 10,027 checks passed, including 10,000 traffic-envelope checks |
| CTest | 5/5: core, help, zero rate, negative count, NaN rate |
| Real TUN integration suite | 5 scenarios / 65 checks passed |
| Shaping demo, 100 probes, 256 kbit/s | 100 delivered and echoed; 0 drops; 3.119 s elapsed |
| Policing demo, 100 probes, 64 kbit/s | 4 delivered and echoed; 96 forward rate drops; no queue |
| Minimum/maximum payloads | 8-byte and 1,400-byte payloads: 10/10 round trips each |
| Run without privileges | Fails at `unshare` before interface creation |
| SIGINT during queueing | Exit 130; remaining 97 queued packets accounted as shutdown drops |
| Existing network state | Compared within the integration session; see `evidence/isolation-check.txt` |

Exact timing and policing counts depend on scheduling. These are recorded
observations, not hard-coded output or promises about another computer.
The default shaping experiment measures 102,800 IP bytes in each direction.

## Deterministic unit coverage

The tests cover initial burst, refill, depletion, idle-time credit capping,
oversized packets, invalid inputs, backward/nonfinite time, FIFO order, queue
overflow, partial-credit waiting, immediate policing, independent directions,
I/O-drop accounting, and pending-queue cleanup. The repeated envelope check
asserts `forwarded_bytes <= burst + rate * elapsed` across 10,000 sample times.

## Real packet integration coverage

The five scenarios exercise actual Linux UDP sockets, IP stacks and TUN devices:

1. Above-limit shaping delivers all 40 probes and echoes without drops; packets
   queue, and forwarding cannot finish before the rate envelope permits it.
2. Above-limit policing drops excess probes and never queues them.
3. A two-packet FIFO tail-drops overflow and drains accepted packets.
4. A short deadline turns pending queued packets into shutdown drops.
5. A fresh low-load run succeeds after earlier endpoint resources are released.

Every scenario checks namespace restoration, payload integrity, no duplication,
packet-accounting conservation, queue capacity, and the byte-rate envelope in
both directions. No custom driver or host firewall changes are needed.

## Reproduce

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build-debug -j 2
ctest --test-dir build-debug --output-on-failure
sudo ./build-debug/traffic_lab self-test
```

For the optimized demonstration, follow the Release instructions in the README.
JSON reports can be generated with `--json NEW_FILENAME.json`.

## Evidence and scope

The `evidence/` directory contains captured output and JSON reports. Local
absolute build paths in CTest output are normalized to `<project>` for sharing.
Raw snapshots of the developer's network addresses are intentionally excluded
from the package; the comparison result is included instead.

WSL can recreate its virtual adapter/address between launches. The relevant
isolation comparison captures the existing network immediately before and
after the experiment in the same Linux session; it does not assume addresses
remain identical across independent WSL launches.

Not verified: a separate VirtualBox/VMware guest, remote GitHub Actions execution,
production traffic, external networks, multi-host throughput, TCP behavior,
IPv6 forwarding, or custom kernel-module development. The project has not been
uploaded to a GitHub account. No results are fabricated for those environments.
