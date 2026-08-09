# ATFP Unpack and GPU Transpose Design

## Status

Approved design for the first ATFP implementation. This document replaces the
TFPA-scatter assumptions in the earlier unpack design where they conflict. It
is the authoritative input to the corresponding implementation plan.

## Goal

Reduce the CPU cost of reassembling unordered per-Station Project VDIF
packets. The receiver continues to write complete `32-byte Project VDIF
header + TFP payload` records into the raw PSRDADA ring. The unpack process
removes packet headers, restores the observation time axis, and publishes
block-scoped ATFP integer-complex data. The GPU then physically transposes and
converts the data to contiguous TFPA CF32 before the existing beamformer.

The resulting path is:

```text
UDP/Project VDIF records
  -> raw PSRDADA ring: unordered records, header + TFP payload
  -> vdif_unpack_worker: parse, validate, time-align, zero-fill
  -> compute PSRDADA ring: payload-only ATFP CI8
  -> H2D
  -> fused [A,Q] -> [Q,A] transpose and integer-to-CF32 conversion
  -> TFPA CF32
  -> existing FPAB beamformer
  -> TFPB beamformed data
  -> optional Power or Stokes
  -> optional TimeIntegrate
```

`Q = T * F * P` for the actual input data block.

## Fixed wire contract

The 32-byte Project VDIF header is not changed.

Each Station represents one antenna selected by `station_id`. Its payload is
physically ordered:

```text
[packet_T, F, P]
```

The current network observation format is fixed to:

```text
PKT_NBIT=16
SAMPLE_FORMAT=CI8
SAMPLE_ENCODING=TWOS_COMPLEMENT
COMPONENT_ORDER=IQ
ENDIAN=LITTLE
```

One complex sample therefore occupies two bytes: one signed 8-bit I component
and one signed 8-bit Q component. I becomes the CF32 real component and Q
becomes the CF32 imaginary component.

The GPU conversion implementation also supports synthetic `CI16` input for
future use and testing, but the first network/unpack implementation does not
advertise CI16 VDIF input support.

`COMPONENT_SIGNED` and `SOURCE_COMPONENT_SIGNED` are removed. Signedness and
negative-value encoding are defined without ambiguity by:

```text
CI8 or CI16 + TWOS_COMPLEMENT
```

## Station-to-antenna contract

Removing the time-keyed `std::map` does not remove the Station-to-antenna
relationship. The observation configuration supplies an ordered Station list:

```text
station_ids = [station_for_A0, station_for_A1, ..., station_for_A(NANT-1)]
```

The list index is the physical A-axis index everywhere after unpack and is the
same A-axis order used by the `F-P-A-B` beamforming weights. Arrival order,
source IP address and source UDP port never select the antenna position.

At transfer initialization the unpacker validates that the Station IDs are
unique and builds a direct lookup table for the 16-bit header field:

```text
station_to_antenna[station_id] = antenna_index
```

Unconfigured entries contain an invalid sentinel. A configured Station lookup
is O(1); an unconfigured Station is discarded and counted without advancing
the observation timeline.

## Observation timeline contract

One group contains the packets from all configured Stations for one packet
time position and one selected frequency segment. Group order is not carried
as a new VDIF header field. The control program supplies the timeline before
starting the transfer.

The raw DADA header contains:

```text
GROUP_PERIOD_PS
GROUP_START_REFERENCE_EPOCH
GROUP_START_SECONDS
GROUP_START_FRAME
EXPECTED_GROUPS
```

First-version constraints are:

- `GROUP_PERIOD_PS` is a positive integer;
- `PKT_TSAMP` retains its existing `us` unit and must be exactly representable
  as an integer number of picoseconds;
- `GROUP_PERIOD_PS = PKT_NSAMP * integer_ps(PKT_TSAMP)`;
- group zero starts on the configured whole-second observation boundary;
- `GROUP_START_FRAME=0`;
- `EXPECTED_GROUPS` is positive and fixed before the transfer starts;
- the control program sends EOD at the exclusive boundary
  `group_index=EXPECTED_GROUPS`.

The sender and unpacker use the same integer-picosecond mapping. For group
ordinal `g`:

```text
elapsed_ps = g * GROUP_PERIOD_PS
seconds = GROUP_START_SECONDS + floor(elapsed_ps / 1_000_000_000_000)
within_second_ps = elapsed_ps mod 1_000_000_000_000
frame = floor(within_second_ps / GROUP_PERIOD_PS)
```

The reference epoch remains `GROUP_START_REFERENCE_EPOCH` for the transfer.
All multiplication, addition and division are checked for integer overflow.
For an incoming packet with a matching reference epoch, define:

```text
delta_seconds = packet.seconds - GROUP_START_SECONDS
first_ordinal_in_second =
  ceil(delta_seconds * 1_000_000_000_000 / GROUP_PERIOD_PS)
candidate_ordinal = first_ordinal_in_second + packet.frame
```

All operations use checked integer arithmetic, including the ceiling division.
The unpacker regenerates `(reference_epoch, seconds, frame)` from the candidate
using the forward mapping above and requires an exact tuple match. This rejects
non-canonical frame values instead of rounding them to a nearby group. A packet
before the start, at or beyond `EXPECTED_GROUPS`, or with an impossible time
tuple is rejected and counted.

This mapping does not require an integer number of groups per second.

## Missing-data and observation-failure semantics

Every configured Station is mandatory. If one sender process fails to start or
exits abnormally, the control program aborts the complete observation transfer.
The receiver-side pipeline must stop; the run is not accepted as a smaller
antenna observation.

Packet loss during an otherwise live observation is handled independently:

- a missing Station packet inside an observed group contributes one zero TFP
  payload at the Station's A position;
- a group for which every Station packet is absent contributes a complete
  zero `[A, packet_T, F, P]` group;
- gaps before the first received packet, between received packets and after
  the last received packet are all representable because the start and
  exclusive stop boundary are known before processing EOD;
- EOD causes the unpacker to emit every remaining group through
  `EXPECTED_GROUPS-1` as data or zero fill;
- a duplicate packet is discarded and counted;
- an unknown Station is discarded and counted;
- an invalid header or invalid-data record is discarded and counted, and its
  expected Station/time position is later zero-filled when resolvable;
- a packet for an already emitted ordinal is late, discarded and counted.

Statistics distinguish at least:

```text
received_records
accepted_packets
invalid_header_packets
invalid_data_packets
unknown_station_packets
duplicate_packets
late_packets
out_of_range_packets
completed_groups
incomplete_groups
fully_missing_groups
missing_station_packets
expected_station_packets
```

A fully missing group increments `incomplete_groups` and
`fully_missing_groups`, and adds `NANT` to both `missing_station_packets` and
`expected_station_packets`.

Zero-filled packet loss does not turn a run into a successful observation when
a configured sender process failed. The test controller owns that distinction.

## Reorder window

The reorder window remains necessary because records are unordered and a group
may span raw ring blocks. The first implementation uses private, bounded host
memory and does not use an uncommitted compute-ring block as the complete
window.

Metadata is indexed by logical group, but the payload data plane is
antenna-major across the complete circular capacity:

```text
payload_window[A, window_capacity_groups, packet_T, F, P]
group_metadata[window_capacity_groups]
```

For logical group ordinal `g` and resolved antenna index `a`:

```text
slot_index = g mod window_capacity_groups
payload_offset =
  (a * window_capacity_groups + slot_index) * packet_payload_bytes
```

An accepted Station packet is copied once from the raw-ring record to this
complete TFP slice. The copy size is exactly `packet_payload_bytes`, not one
sample and not one polarization. This layout makes the logical groups for one
antenna contiguous except at the single circular wrap boundary.

Each metadata slot stores:

```text
owned_ordinal
active
seen_station_bitmap
accepted_station_count
```

The ordinal tag prevents modulo aliasing. The bitmap and count are cleared when
a metadata slot is reused, but the payload plane is not pre-zeroed. Before an
output range is copied, only Station slices not marked present for the expected
ordinal are zeroed. A completely absent group therefore produces `NANT` zero
slices even though no active metadata slot was ever created for it. This avoids
stale data without adding a redundant full-window clear to loss-free traffic.

The window does not use `std::map<VdifGroupKey,...>` on the production fast
path. It tracks:

```text
next_emit_ordinal
highest_seen_ordinal
groups_per_compute_block
window_capacity_groups
reorder_horizon_groups =
  window_capacity_groups - groups_per_compute_block
```

The geometry is validated before allocating memory:

```text
groups_per_compute_block =
  compute_block_bytes / (NANT * packet_payload_bytes)
window_capacity_groups =
  configured_window_blocks * groups_per_compute_block
configured_window_blocks >= 2
```

The division must be exact, all products must fit the configured memory limit,
and `groups_per_compute_block` must be positive. A validated, in-range packet
from a configured Station may advance `highest_seen_ordinal`. Unknown Stations,
invalid headers and out-of-range packets cannot advance the watermark.

A group is final when all configured Stations have arrived, or when:

```text
highest_seen_ordinal - group_ordinal >= reorder_horizon_groups
```

A full output block becomes ready only when every group in its consecutive
range is final. Complete groups can become final before the watermark; missing
and incomplete groups wait for the watermark or EOD. This group-distance rule
does not depend on how many packets happen to fit in a raw-ring block.

If an otherwise valid packet is too far ahead to fit, the unpacker repeatedly
finalizes and publishes the oldest output block ranges, zero-filling unresolved
positions, until the packet's slot is safe. It records the large gap/window
advance. A calculated slot with a different live ordinal must never be
overwritten silently. A packet older than `next_emit_ordinal` is late and is
discarded.

At EOD, the unpacker finalizes and publishes every remaining ordinal through
`EXPECTED_GROUPS-1`. Packet lookup and slot selection are O(1) after header
validation and exact timeline-to-ordinal conversion.

The first implementation remains single-threaded. No mutexes or parallel
writes are added to the window.

## ATFP compute block assembly

A VDIF-specific `AtfpBlockWriter` is introduced under the unpack module. The
generic `pipeline::GroupBlockWriter` retains its existing append semantics and
is not changed into an ATFP-specific component.

For an output compute block containing `G` groups, Station `a`, and local group
offset `g`, the destination is:

```text
block + a * (G * packet_payload_bytes) + g * packet_payload_bytes
```

The writer acquires exactly one PSRDADA writable data block only after the
complete output range is final. It fills that one block antenna by antenna. If
the logical range does not cross the private circular boundary, one continuous
source segment is copied for each antenna. If it crosses the boundary, that
antenna plane is split into two disjoint source and destination segments. The
two operations do not duplicate any group: each source group maps to exactly
one target group position and every target byte is written exactly once.

Therefore the first implementation performs these two data-placement stages:

```text
one packet-payload copy: raw record -> private antenna/group position
one ordered publication copy per byte: private window -> compute-ring block
```

This deliberately replaces thousands of sample-sized copies with two
payload-sized copies while retaining a simple and testable reorder boundary.
A compute-ring-direct optimization is deferred until profiling shows that the
second large copy is material.

The performance statement "one 4096-byte copy per packet" refers specifically
to raw payload placement into the private reorder window. Publishing an
ordered group into the PSRDADA compute block is the second payload-sized copy.
Tests and benchmark reports must not describe the complete raw-to-compute path
as a one-copy path.

The writer never holds two compute-ring blocks simultaneously. Its lifecycle
for every block is:

```text
wait until an output group range is final in private memory
acquire one writable compute block
fill that block completely, including required zero slices
commit that block exactly once
release/reuse the corresponding private window positions
```

It must not acquire the compute block and then wait for more network packets.
If the compute ring has no writable block, the acquire call supplies normal
PSRDADA backpressure and the wait duration is measured separately.

Full blocks use `G=Gmax`. If EOD produces `G < Gmax`, the destination offset is
computed with the actual `G`, so the final block is written directly in compact
ATFP layout without an intermediate full-stride layout or an in-place
compaction pass:

```text
committed_bytes = A * G * packet_payload_bytes
```

No padding or antenna-plane holes are published. Missing Station positions are
valid zero samples and remain part of the committed tensor. A block acquisition
failure or commit failure fails the transfer; an uncommitted block is never
reported as published data.

## Block-scoped ATFP metadata

Every compute data block is an independent tensor. Concatenating two blocks
does not create a single global ATFP tensor; the file/ring layout is:

```text
Block0[A,T0,F,P] | Block1[A,T1,F,P] | ...
```

The unpack output header contains:

```text
DATA_STAGE=UNPACKED
ORDER=ATFP
LAYOUT_SCOPE=BLOCK
SAMPLE_FORMAT=CI8
SAMPLE_ENCODING=TWOS_COMPLEMENT
COMPONENT_ORDER=IQ
COMPONENT_NBIT=8
SAMPLE_NBIT=16
ENDIAN=LITTLE
NANT
NCHAN
NPOL
BLOCK_NTIME
OUTPUT_BLOCK_BYTES
RECORD_HEADER_BYTES=0
RESOLUTION=NANT*NCHAN*NPOL*2
RECORD_BYTES=OUTPUT_BLOCK_BYTES
TRANSFER_SIZE=EXPECTED_GROUPS*NANT*packet_payload_bytes
```

`BLOCK_NTIME` is the nominal full-block T. For an actual data block:

```text
actual_T = actual_block_bytes / (NANT * NCHAN * NPOL * 2)
```

The block is rejected when the division has a non-zero remainder. A partial
final block uses its actual T. `FILE_SIZE` and `OBS_OFFSET`, when present, are
required to align to the nominal physical ATFP block boundary. Byte-rate and
transfer fields retain their sample/time meaning and are checked for overflow.

The packet-format profile describes only the wire header and TFP payload. Its
`payload.output_order` field is removed because ATFP is an unpack-stage output
contract rather than a property of the FPGA packet. The packet-format schema,
portable parser, inspect tool, fixtures and tests are updated together. The
unpack output order is declared only by the unpack configuration/header
contract in this document.

## Raw-ring EOD prerequisite

Packet loss makes the accepted raw record count unlikely to be a multiple of a
full raw-ring block. Before fault-injection acceptance, `rdma2dada` must commit
an EOD partial raw block containing every complete received VDIF record.

The partial raw block must:

- contain only complete `header + payload` records;
- report its actual byte count to the PSRDADA reader;
- include completed receive work requests that belong to the transfer;
- preserve receiver accepted/published accounting;
- reach the unpack worker before EOD;
- leave no unpublished WR-depth tail attributed incorrectly to network loss.

This prerequisite is tested independently from ATFP output ordering.

## GPU physical transpose and conversion

After H2D, one module consumes block-scoped `ATFP/CI8` or synthetic
`ATFP/CI16`. For actual block T, define:

```text
Q = T * F * P
src(a,q) = a * Q + q
dst(q,a) = q * A + a
```

The CUDA backend physically writes contiguous `[Q,A]`, which is TFPA. It does
not return a stride-only view. The same kernel also:

- reads I/Q as signed two's-complement CI8 or little-endian CI16;
- maps I to real and Q to imaginary;
- multiplies each component by the configured positive finite
  `CONVERSION_SCALE` exactly once;
- writes CF32;
- runs on the caller-owned non-default CUDA stream;
- does not synchronize internally.

The implementation uses a padded shared-memory tiled transpose so ATFP reads
and TFPA writes are coalesced where geometry permits. Non-square matrices,
dimensions smaller than a tile, non-tile multiples and partial final blocks
must be supported.

The conversion output header contains:

```text
DATA_STAGE=CONVERTED
ORDER=TFPA
SOURCE_ORDER=ATFP
SAMPLE_FORMAT=CF32
SOURCE_SAMPLE_FORMAT=CI8 or CI16
COMPONENT_ORDER=RI
COMPONENT_NBIT=32
SAMPLE_NBIT=64
CONVERSION_SCALE
MEMORY=CUDA_DEVICE or HOST
```

The existing beamformer continues to consume TFPA CF32 with FPAB weights and
produce TFPB CF32. Power, Stokes and TimeIntegrate public layouts do not
change.

## Legacy TFPA correctness reference

The existing CPU TFP-to-TFPA scatter implementation is retained as a
correctness reference while the ATFP fast path is developed. It is not the
default production data path after ATFP acceptance.

Given the same validated packet sequence, tests produce:

```text
legacy_result = legacy CPU unpack -> TFPA integer bytes
fast_result = ATFP unpack -> independent CPU ATFP-to-TFPA oracle
```

`legacy_result` and `fast_result` must be byte-identical for CI8 input,
including arbitrary Station arrival, zero-filled missing packets, completely
missing groups, full blocks and compact final partial blocks.

The GPU comparison additionally checks:

```text
ATFP unpack -> GPU transpose/convert -> TFPA CF32
legacy TFPA integer bytes -> reference complex conversion -> TFPA CF32
```

Every complex component must match under the documented FP32 tolerance, and
scale must be applied exactly once. The legacy reference may be exposed through
a test/diagnostic backend or a test-linked helper, but ordinary observation
configuration must select the ATFP path after it passes acceptance.

## Performance observability

Correct output alone is not real-time acceptance. Release benchmarks report
the following boundaries separately:

```text
header decode and validation
timeline ordinal calculation and circular-slot lookup
raw payload bytes copied into the private window
ordered ATFP bytes copied into the compute-ring block
time waiting to acquire an output ring block
time committing an output ring block
total ConsumeRawBlock time
total transfer throughput
```

The fast path records counts and bytes continuously but must not call a high
overhead clock for every packet in production mode. Fine-grained phase timing
uses a benchmark/diagnostic build or statistically sampled packets. Normal
runtime timing is aggregated at raw-block and compute-block boundaries.

Reports include at least:

```text
records and groups processed
payload copy count and bytes
compute-ring copy count and bytes
raw blocks per second
payload Gbps
wall-clock throughput
ring-acquire wait fraction
CPU core and NUMA placement
```

Release unbound, Release NUMA-bound legacy TFPA, and Release NUMA-bound ATFP
results use the same packet geometry and sender rate so parsing, placement,
ring waiting and total throughput can be compared without conflating build or
affinity changes.

## pipeline_worker composition

The CUDA process becomes:

```text
ATFP integer host ring
  -> H2D
  -> physical transpose + complex conversion
  -> beamform
  -> optional Power or Stokes
  -> optional TimeIntegrate
  -> D2H
  -> output host ring
```

The worker configuration supplies `conversion.scale`. Scratch planning must
reserve distinct regions for converted TFPA, beamformed TFPB and any
unintegrated product. All capacities are derived from the actual input block T
with overflow checks. H2D and D2H remain layout-agnostic byte transfers.

The first implementation uses the existing single worker CUDA stream and
per-block synchronization. Double buffering and copy/compute overlap are
deferred until the single-stream path is correct and profiling identifies a
transfer bubble.

## CPU execution and affinity

The first ATFP unpack implementation remains one data-path thread. The worker
does not hard-code a CPU number and does not call Linux affinity APIs.

The control/test program launches receiver and unpack with `taskset` on
different physical cores belonging to the NIC's NUMA node. Phase 0 determines
the actual server topology before choosing the core IDs. Acceptance records
both an unbound Release baseline and a bound Release result. If optimized ATFP
single-core throughput remains insufficient, multithreaded header parsing and
payload placement becomes a separate measured design; it is not added by
putting locks around the current maps and writer.

## Development and test loop

Work proceeds in small behavior changes. For every completed module or
behavior change:

1. implement the smallest testable change;
2. run portable/local correctness tests and debug failures;
3. notify the existing `GPU服务器代码测试` task with affected files, source
   hashes, build commands, fixtures and acceptance criteria;
4. the testing task runs Phase 0 before changing any remote harness, syncs by
   SHA256, builds and tests on the server, and explicitly sends the result back
   to the originating development task;
5. the development task waits for the callback and classifies PASS,
   PRODUCT_FAIL, HARNESS_FAIL or ENV_BLOCKED independently from cleanup and
   notification status;
6. on failure, modify only after using the returned evidence, then repeat the
   same server test;
7. after explicit server PASS, remind the user that the tested change is ready
   for a Git decision; do not commit or push automatically.

Final functional acceptance requires three consecutive clean executions.
Every performance point requires one warm-up followed by at least three
measured runs. Server-side Git is prohibited.

## Required acceptance coverage

Portable unpack tests cover:

- exact ATFP bytes with asymmetric A/T/F/P extents;
- arbitrary Station arrival order;
- groups crossing raw blocks;
- duplicate, unknown, invalid, late and out-of-range packets;
- one missing Station;
- every Station missing the same group;
- missing leading, internal and trailing groups;
- a full compute block and compact partial EOD block;
- consecutive transfers in one worker;
- overflow and memory-limit rejection.

They also require byte-for-byte equivalence between the retained legacy TFPA
reference and ATFP followed by the independent CPU transpose oracle.

GPU tests cover:

- CI8 and synthetic CI16;
- negative two's-complement extrema;
- conversion scale applied exactly once;
- non-square and non-tile-multiple `[A,Q]` matrices;
- partial T;
- exact comparison with the CPU oracle;
- H2D -> transpose/convert -> beamform -> selected product -> integration ->
  D2H using known payloads and FPAB weights.

PSRDADA/network acceptance covers:

- three fresh ring/process lifecycles at low rate with dbdisk byte inspection;
- Release unbound versus NUMA-local bound unpack comparison;
- raw partial-block EOD publication;
- one warm-up plus three measured dbnull runs per rate;
- rate progression only while every lower correctness gate passes;
- refinement between the last passing and first failing coarse rate;
- payload rate and estimated Ethernet/IP/UDP/VDIF wire rate reported
  separately;
- per-run NIC/CQ deltas, ring occupancy, CPU/NUMA and component progress;
- deterministic `0.001%`, `0.01%` and `0.1%` fault injection below the stable
  rate;
- final full pipeline numerical validation, not only ATFP byte validation.

Performance evidence separates parser, circular-slot lookup, payload-copy,
compute-ring wait and total transfer time so a functionally correct but
non-real-time implementation cannot pass unnoticed.

## Deferred optimizations

The following are explicitly outside the first implementation:

- changing the 32-byte Project VDIF header;
- direct raw-ring payload writes into uncommitted compute-ring blocks;
- zero-copy ownership transfer between private memory and PSRDADA blocks;
- multithreaded unpack;
- CUDA direct ATFP beamforming without a physical TFPA buffer;
- multiple CUDA streams or double-buffered block overlap;
- network CI16 enablement;
- runtime modification of an already-started transfer boundary through
  `pipelinectl`.

These items require profiling or a separate control-plane design before they
are authorized.
