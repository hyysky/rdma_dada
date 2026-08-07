# High-throughput VDIF sender design

## Goal

Extend `fpga_sender_sim` from a deterministic functional sender into a Linux
test sender that can drive several equal-rate Station streams toward the fixed
qths1 receive endpoint, measure the all-valid performance limit, and later run
low-rate fault injection at that limit. Station identity remains exclusively
the Project VDIF `Station ID`; source ports only make the simulated topology
closer to independent FPGA flows.

## Scope

This design covers the sender and controller behavior required by Task 8B-D:

- explicit source IPv4 address and UDP port per Station process;
- equal per-Station rate derived from one aggregate receive-rate target;
- Linux `sendmmsg()` batch transmission with reusable buffers;
- batch-level monotonic-clock pacing and machine-readable statistics;
- a correct-packet rate sweep followed by reproducible low-rate fault runs;
- UDP sender-side bottleneck identification and tuning when the requested rate
  cannot be sustained.

It does not change the Project VDIF v1 wire contract, Station-to-antenna
mapping, `rdma2dada`, `vdif_unpack_worker`, PSRDADA block geometry, CUDA
algorithms, or the fixed destination `174.0.1.111:1000` used by the current
server tests. It will not implement or evaluate a libibverbs sender: the test
source must preserve the UDP transmission model that the final FPGA can
realistically provide.

## Selected transport approach

The first implementation uses connected IPv4 UDP sockets and Linux
`sendmmsg()`. Each `fpga_sender_sim` process owns one socket and represents
exactly one Station ID. The process binds its configured source IP and source
port, connects to the common destination, prepares a reusable packet batch,
and submits that batch with one `sendmmsg()` call.

This approach is preferred over `SO_TXTIME` because it does not require ETF
qdisc configuration or additional system privileges. If the two sender hosts
cannot sustain the requested aggregate rate, development remains on UDP and
measures the limiting factor before tuning batch size, socket buffers, CPU
affinity, NUMA placement, packet size, and sender-process placement. A lower
measured ceiling is reported as the UDP/host result rather than hidden by
switching to another transport.

macOS keeps a portable single-message fallback for unit and loopback tests.
The Linux batch path is selected at compile time and is the only path used for
server performance acceptance.

## Rate semantics

The HF controller accepts one aggregate target for the qths1 receive NIC. It
counts the configured Station processes and assigns every Station the same
rate:

```text
station_target_gbps = aggregate_target_gbps / station_count
```

There are no Station weights and no per-Station override. A four-Station,
40-Gbps aggregate run therefore configures every process for 10 Gbps. If the
aggregate rate is not exactly divisible in decimal representation, the
controller writes the same full-precision quotient to every Station config;
the reported aggregate target is the sum of those identical values.

`target_gbps` means UDP payload bytes per second, not Ethernet wire rate. Test
reports also record NIC byte counters and an estimated Ethernet-frame rate so
payload, NIC, and wire measurements are never conflated.

## Configuration contract

New high-throughput configurations use schema version 2. Schema version 1
continues to load with its current behavior: the OS selects the source address
and port, `BURST` and `REALTIME` use one `send()` per record, and the existing
deterministic payload formula remains unchanged.

Schema version 2 adds these required objects:

```json
{
  "schema_version": 2,
  "source": {
    "ip": "174.0.1.100",
    "port": 41001
  },
  "destination": {
    "ip": "174.0.1.111",
    "port": 1000,
    "path_mtu": 9000
  },
  "station": {
    "station_id": 101
  },
  "transmit": {
    "target_gbps": 10.0,
    "batch_packets": 16,
    "payload_mode": "REPEAT_TEMPLATE"
  }
}
```

The existing `packet`, `time`, and deterministic fault-list objects remain
required and retain their current field meanings. Version 2 validation rules
are:

- `source.ip` is a numeric, non-wildcard IPv4 address;
- `source.port` is in `1..65535`;
- destination port remains an ordinary configurable field in the executable,
  while the Task 8 controller fixes it to `1000`;
- `time.mode` is `PACED` and `time.start_utc` is a common future barrier for
  all Station processes;
- `target_gbps` is finite and greater than zero;
- `batch_packets` is in `1..64`;
- the UDP record, including the 32-byte VDIF header, fits `path_mtu - 28`;
- `payload_mode` is `REPEAT_TEMPLATE` for performance runs or
  `DETERMINISTIC` for functional runs.

The repository will include a schema-v2 example but will not rewrite the
schema-v1 example used by existing tests.

## Packet construction and buffer ownership

The sender allocates one batch pool during startup. Each slot contains one
complete UDP datagram and one Linux `mmsghdr`/`iovec` descriptor. No allocation,
vector growth, JSON parsing, or socket setup occurs in the send loop.

For `DETERMINISTIC`, the existing `BuildVdifSenderRecord()` behavior is
preserved and remains the correctness oracle. It is intended for low-rate
functional runs.

For `REPEAT_TEMPLATE`, the payload is generated once per Station and reused.
Before every submission, the sender updates the 32-byte VDIF header for the
slot's unique group index. The header time fields therefore advance exactly as
defined by `sample_interval_ps`, even though the payload pattern repeats across
time. Repeating payload is valid observational test data and avoids making
payload generation the throughput bottleneck. Station IDs still produce
different templates so TFPA antenna slices remain distinguishable.

After `sendmmsg()` returns, all messages reported as sent are retired. A short
batch result retries the unsent suffix unless the socket returned a permanent
error. A datagram can never be partially transmitted by UDP; any result that
does not account for a complete message is treated as an error.

## Pacing

Pacing uses `CLOCK_MONOTONIC` and an absolute byte budget. For every batch, the
sender computes the ideal deadline from cumulative scheduled payload bytes:

```text
deadline_ns = start_ns
            + cumulative_payload_bytes * 8 / (target_gbps * 1e9)
```

The calculation uses checked integer/fixed-point arithmetic for cumulative
bytes and nanoseconds; repeated floating-point addition is not used. When the
sender is ahead of the deadline it sleeps for the coarse portion and performs
a short final busy wait. When it is behind, it sends the next batch immediately
and increments an `overrun_batches` counter. It never drops a valid record only
to catch up with the configured rate.

`batch_packets` controls the syscall/latency trade-off. The initial server
value is 16 and may be changed between benchmark runs without changing packet
or Station semantics.

## Statistics

Each process prints human-readable progress once per second and one final
single-line JSON object. The final object contains:

- Station ID, bound source endpoint, and destination endpoint;
- scheduled, successfully sent, retried, and failed packet counts;
- payload bytes sent;
- elapsed monotonic nanoseconds;
- target and actual payload Gbps;
- actual packets per second;
- batch count, short-batch count, and pacing-overrun count;
- selected batch size and payload mode.

The process exits non-zero if socket setup fails, any permanent send error
occurs, or the final sent count differs from the configured count. The
controller applies the 2% target-rate acceptance rule to sufficiently long
performance runs. It may use a wider aggregate tolerance when NIC or receiver
limits are intentionally being located, but it must report that distinction.

## HF controller and source ports

The controller runs on HF and independently connects to qths1, qtpulsar1, and
qtpulsar2. It never requires qths1 to SSH to a sender. It assigns a unique
source port to every Station process, creates a common future start barrier,
starts receiver-side readers before writers, starts all Station processes, and
collects their JSON summaries plus qths1 NIC/RDMA/unpack logs.

Source IP and port are diagnostics only. The receiver flow continues to match
only destination MAC, destination IP, and destination port. Unpack continues
to use only the VDIF Station ID and configured `antenna_map` for the A axis.

## Two-stage server test

### Stage 1: all records valid

Run aggregate payload targets of 1, 5, 10, 20, 30, 35, and 38 Gbps. If 38 Gbps
is stable, add finer points approaching 40 Gbps. Every scheduled packet is a
valid Project VDIF record; there are no missing, duplicate, reordered,
wrong-length, unknown-Station, or invalid-header records.

Each point records sender summaries, qths1 NIC counters, RDMA accepted and
wrong-length counts, raw-ring throughput, unpack statistics, compute-ring
throughput, CPU utilization, and NUMA placement. The result identifies both
the highest stable all-valid rate and the first component that saturates.

### Stage 2: low-rate faults at the measured limit

Stage 2 uses the highest stable Stage-1 aggregate rate, not an assumed 40 Gbps.
Total injected fault rates are 0.001%, 0.01%, and 0.1% of scheduled logical
records. A fixed seed produces an explicit manifest. Each fault type is first
run alone; a mixed run then divides the same total fault budget evenly among
enabled types so the total is never multiplied by the number of types.

The types are wrong length, unknown Station, duplicate, bounded reorder,
invalid VDIF header, and omitted group. The fault implementation is a later
Task 8D change built on the sender interfaces in this document. Stage 1 does
not wait for that implementation.

## Testing strategy

Development follows TDD:

1. Portable config tests first reject missing/invalid source, rate, batch, and
   payload-mode fields, then verify schema-v1 compatibility.
2. Portable pacing tests use a fake monotonic clock and assert exact deadlines,
   overflow rejection, equal-rate calculations, and overrun accounting without
   sleeping in tests.
3. Packet-pool tests verify unique advancing VDIF headers, fixed Station ID,
   repeatable Station-specific payload, and zero allocation after startup.
4. Linux loopback tests bind two explicit source ports, receive both streams,
   decode all headers, and verify sender JSON counters.
5. qths1/qtpulsar1/qtpulsar2 repeat the low-rate end-to-end baseline before
   beginning the all-valid rate sweep.

Every completed behavior is handed to the `GPU服务器代码测试` task with the
affected targets, configs, commands, and acceptance criteria. macOS tests are
development evidence only.

## Acceptance criteria for Task 8B

- Existing schema-v1 sender tests remain unchanged and pass.
- Schema-v2 configs bind the requested source IP and port.
- Two local/server Station processes can use distinct source ports and the
  same destination port without affecting Station ID decoding.
- Linux sends batches through `sendmmsg()` with reusable buffers.
- VDIF headers advance uniquely and decode correctly in both payload modes.
- The measured per-process payload rate is within 2% of its target at rates the
  host can sustain, with no permanent send error.
- Final JSON statistics reconcile packet and byte counts exactly.
- The existing RDMA-to-unpack low-rate baseline remains bit-correct.
- Server testing leaves no process, ring, temporary capability, or temporary
  test file behind.
