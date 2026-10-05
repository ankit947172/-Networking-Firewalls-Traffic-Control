# Trainer demonstration: 5–10 minutes

Build and test in the Linux VM beforehand. Understand the code and change rates
and queue sizes yourself before presenting. Do not describe the project as a
custom kernel module or as all six Domain 3 projects.

## Walkthrough

**Minute 0–1:** Explain: "This C++ Linux gateway uses the TUN driver and isolated
network namespaces. A token bucket limits bandwidth; shaping queues excess
packets and policing drops them." Show the README architecture diagram.

**Minute 1–3:** Demonstrate shaping:

```sh
sudo ./build/traffic_lab lab --mode shape --rate-kbps 256 --packets 100
```

Identify delivered packets, replies, queue peak and delay. The offered rate is
about 4,112 kbit/s, limited to 256 kbit/s per direction. All 100 probes should
arrive when there is sufficient queue capacity/time; duration is about 3.1 s.
Explain the refill formula and initial burst.

**Minute 3–4:** Demonstrate policing:

```sh
sudo ./build/traffic_lab lab --mode police --rate-kbps 64 --packets 100
```

Show rate drops and zero queued packets. Exact drop counts depend on timing.
Remaining packets arrive promptly because they do not wait in a policy queue.

**Minute 4–5:** Demonstrate bounded memory:

```sh
sudo ./build/traffic_lab lab --mode shape --rate-kbps 64 --queue-packets 2
```

Show a queue peak of 2 and nonzero queue drops. Explain the memory/latency tradeoff.

**Minute 5–7:** Open `create_endpoint`, `read_packets`, and `inject` in
`src/linux_lab.cpp`. Explain device open, `TUNSETIFF`, nonblocking reads/writes,
`poll`, network isolation, and RAII cleanup.

**Minute 7–9:** Show tests:

```sh
ctest --test-dir build --output-on-failure
sudo ./build/traffic_lab self-test
```

Explain the five real-packet scenarios and the difference between deterministic
algorithm tests and actual driver integration tests. Recorded development
results are in `evidence/`; the VM produces fresh results.

## Questions to prepare for

| Question | Answer |
| --- | --- |
| Shaping vs policing? | Queue and delay excess packets versus immediate drops. |
| Why token bucket? | It accounts for packet size, permits bounded bursts, and caps idle-time credit. |
| Why not sleep after every packet? | Packet sizes vary and OS scheduling is imprecise; credit uses actual elapsed time. |
| What are the units? | CLI: decimal kbit/s. Bucket: bytes/s. Counted bytes include IP/UDP headers. |
| Why burst >= MTU? | Otherwise a large packet could never collect enough credit to pass. |
| Why a monotonic clock? | Wall-clock adjustments must not alter bandwidth credit. |
| Is this a custom driver? | No: it implements driver interaction with Linux's existing TUN device from C++. |
| How is the laptop isolated? | The test runs in virtual Linux; endpoints have private namespaces with no physical links/default routes. |
| Do namespaces have separate kernels? | No. They isolate networking while sharing the VM's Linux kernel. |
| Why can a short test exceed the average rate? | The initial burst adds credit: the bound is burst + rate × time. |
| What happens when the queue is full? | The newly arriving packet is dropped; queued packets retain FIFO order. |
| What happens on Ctrl+C? | A signal flag stops the loop, queued packets are accounted, and RAII releases descriptors. |
| What are the limitations? | IPv4 UDP lab traffic, ordinary scheduling, no per-flow fairness/encryption/production validation. |
| Possible extensions? | TCP validation, per-flow queues, priorities, and a separate gateway service interface. |

Review `include/domain3/traffic_controller.hpp` carefully: it is the policy
implementation your friend should be able to explain line by line.
