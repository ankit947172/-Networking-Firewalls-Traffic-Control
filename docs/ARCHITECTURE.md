# Architecture and algorithm

## Problem and modules

Applications can offer packets faster than a network path should carry them.
Sending everything causes congestion; retaining everything indefinitely can
exhaust memory. This project implements explicit queueing and dropping policies.

`TokenBucket` handles credit accounting. `TrafficController` combines credit,
a FIFO, and an output callback. The Linux adapter owns driver and socket I/O.
The lab orchestrates isolated endpoints; the CLI validates parameters. This
separation lets tests replace the real driver with an in-memory packet sink.

## Token bucket

With rate `R` bytes/s, burst `B` bytes, tokens `T`, and monotonic elapsed time `dt`:

```text
T = min(B, T + R * dt)
if packet_bytes <= T:
    T -= packet_bytes
    admit the entire packet
else:
    queue the packet (shape) or drop it (police)
```

The bucket starts with `B` tokens; one token means one IP byte. Capping credit
prevents unlimited bursts after idle time. Fractional tokens preserve small
refills. `steady_clock` avoids wall-clock jumps; tests supply time directly.
For traffic starting at zero, accepted bytes obey `A(t) <= B + R*t`. A full
bucket at any interval's start gives the corresponding burst-plus-rate bound.

The runtime requires burst capacity >= the 1,500-byte MTU. A larger packet could
never fit in the bucket; the core explicitly drops such packets. Packets are
atomic and never partially forwarded. Decimal kbit/s becomes bytes/s using
`rate_kbps * 1000 / 8`.

## Queue and drop behavior

Shaping drains older packets first. With an empty FIFO and enough credit, a new
packet goes directly to the sink. Otherwise it joins the queue. At capacity,
the incoming packet is tail-dropped. Policing has no queue: admission is immediate.

Each direction has its own controller, so the configured rate is per direction.
Temporary output backpressure produces an explicit I/O drop; already consumed
tokens remain spent, conservatively preserving the rate envelope. Other I/O
errors stop the run. Pending packets become shutdown drops on exit.

```text
received = forwarded + rate_drops + queue_drops + oversized_drops
           + io_drops + shutdown_drops + currently_queued
```

## Namespace/driver lifecycle

1. Save a descriptor to the original network namespace.
2. Create a fresh endpoint namespace with `unshare(CLONE_NEWNET)`.
3. Verify namespace identity differs from the original using device/inode IDs.
4. Open `/dev/net/tun`; use `TUNSETIFF` with `IFF_TUN | IFF_NO_PI | IFF_TUN_EXCL`.
5. Configure only the new interface and bind its UDP socket.
6. Restore the original namespace using `setns`.
7. Repeat for the second endpoint and verify restoration.
8. Operate both endpoints through their retained file descriptors.
9. On exit, close sockets, TUN descriptors, and namespace handles using RAII.

TUN carries IP packets; TAP would carry Ethernet frames. `IFF_NO_PI` omits the
extra packet-information header. `IFF_TUN_EXCL` requires a new interface.
No `TUNSETPERSIST`, named namespace mounts, forwarding sysctls, or shell-based
network setup are used. The `/30` addresses create connected routes inside the
new namespaces. The userspace gateway transfers packets between their stacks.

On partial setup failure, owned resources close automatically. A restoration
guard attempts to restore the original namespace before reporting the failure.
If restoration fails, the process exits rather than attempting fallback setup.

## Event loop and measurement

The loop emits due probes in bounded batches, reads the left TUN, applies the
forward controller, injects into the right TUN, receives and echoes at the right
socket, and repeats the policy in the opposite direction for replies. `poll`
waits up to 1 ms for activity between iterations. Batch limits prevent the
generator from indefinitely starving packet forwarding.

Probes contain a magic value, network-byte-order sequence number, and known
payload pattern. Linux supplies the IP/UDP headers. The receiver validates the
payload, detects duplicates, and associates echoes with monotonic send times.
The gateway checks basic IPv4 version/header/total length. Non-IPv4 frames are
ignored and counted. It does not claim deep protocol validation.

SIGINT/SIGTERM handlers only set a `sig_atomic_t` flag. Normal control flow
accounts for pending packets, prints results, and releases resources. A deadline
returns status 2; interruption returns 130.

## Resource bounds and scope

Bucket arithmetic and FIFO push/pop are constant-time; copying a packet is
linear in its length. Queue memory scales with capacity times packet size,
independently in both directions. CLI bounds are 8,192 queued packets, 10,000
probes, 1,400-byte UDP payloads, and 120 seconds per experiment.

This is C++ userspace policy connected to the existing TUN driver. It does not
implement a custom driver or kernel-resident rate limiter. Its architecture is
appropriate for demonstrating OS interfaces and traffic-control principles;
production gateway deployment is outside the tested scope.
