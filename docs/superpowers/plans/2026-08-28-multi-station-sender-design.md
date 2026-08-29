# Multi-Station FPGA Sender Simulation Design

## Scope

Extend only the repository's FPGA/UDP sender simulator and Task 8C controller
so two physical sender hosts can represent an arbitrary Observation Station
list. Production `rdma2dada`, VDIF unpack, PSRDADA headers, `pipeline_worker`
and CUDA algorithms remain unchanged.

## Fixed production-pressure geometry

- `NANT=469`, `NCHAN=4`, `NPOL=1`, `NBEAM=350`.
- Every channel represents exactly 1 MHz; CI8 complex input therefore carries
  `469 * 4 * 1 * 2 * 8 * 1 MHz = 30.016 Gbit/s` payload.
- qtpulsar1 receives the first 235 Observation Station IDs: its astronomical
  sample payload is 15.040 Gbit/s and its 4128-byte VDIF-record target is
  15.1575 Gbit/s. qtpulsar2 receives the remaining 234 IDs: 14.976 Gbit/s
  samples and 15.093 Gbit/s VDIF records. Total sender traffic is therefore
  30.2505 Gbit/s while the signal payload remains 30.016 Gbit/s.
- Each host owns one fixed source IP/port and one UDP socket. Station identity
  exists only in the Project VDIF header.

## Sender contract

Schema versions 1 and 2 remain byte-for-byte compatible. Schema version 3 is
the paced multi-Station form and replaces `station.station_id` with the
non-empty, duplicate-free `station.station_ids` array.

`time.group_count` remains the number of astronomical time groups, not the
number of UDP records. A schema-v3 sender schedules
`group_count * len(station_ids)` records. Within group `g`, every local Station
is emitted exactly once with the same VDIF time key. The first Station rotates
by `g % len(station_ids)`, removing a permanent early/late Station bias while
preserving deterministic order.

Pacing uses the total host payload rate and the flattened record stream. It
does not sleep once per Station. Existing `sendmmsg` batching and the 64-record
batch limit remain in force.

The final sender JSON reports the complete Station list, per-Station scheduled
and sent counts, aggregate record/byte counts, fixed source endpoint, and
pacing timestamps. Any non-zero sender exit aborts the whole observation.

## Controller contract

The controller reads the exact Station IDs from the compiler-resolved
Observation. For more than one Station it deterministically partitions the
ordered list into `ceil(N/2)` and `floor(N/2)` entries for qtpulsar1 and
qtpulsar2. It creates exactly two sender processes and keeps the accepted fixed
source ports. For a one-Station receive diagnostic it preserves the existing
single-sender behavior.

Each host target rate is derived from its Station share, not forced to half of
the aggregate. Acceptance verifies the exact Station list, per-Station count,
aggregate count, rate, source endpoint and `SENDMMSG` backend.

## Validation

Portable tests cover schema compatibility, duplicate/range rejection,
time-major rotated ordering, exact packet accounting, deterministic controller
partitioning and proportional rates. Linux loopback verifies that one socket
emits multiple Station IDs. Server acceptance first uses a small multi-Station
full-chain fixture, then one 30.016 Gbit/s diagnostic, followed only on success
by a 60-second warm-up plus three measured runs.
