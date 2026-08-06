# VDIF Unpack and Sender Simulation Design

## 1. Scope

This change completes the first functional path from the raw packet ring to an
unpacked compute ring and supplies a software sender for testing without the
FPGA:

```text
UDP sender simulators
  -> rdma2dada
  -> raw PSRDADA ring
  -> vdif_unpack_worker
  -> compute PSRDADA ring
```

The first version targets correctness rather than line rate. It has no CUDA
code. GPU conversion and algorithms remain downstream of the compute ring.

## 2. Fixed transport and observation assumptions

- The RDMA receive flow matches only the destination MAC address, destination
  IPv4 address and destination UDP port. Source MAC, source IP and source port
  are unrestricted.
- Each sender process represents exactly one configured numeric Station ID.
  Multiple servers send different Station IDs to the same receiver endpoint.
- All senders use the same Project VDIF profile, observation geometry and VDIF
  time fields. Sender startup jitter does not change group identity.
- One unpack worker handles one fixed contiguous frequency segment. The
  `first_channel_id`, `NCHAN`, `NPOL`, `NSAMP` and sample format are constant
  during one transfer and must match the raw-ring header and JSON configuration.
- The wire record is fixed-size Ethernet/IPv4/UDP carrying one 32-byte Project
  VDIF v1 header followed by one TFP payload. VLAN, IPv6, IP options and IP
  fragmentation are outside the first version.
- The UDP simulator checks that its datagram fits the selected path MTU and
  enables do-not-fragment behavior. An oversized configuration fails before
  transmission.

## 3. Packet group

One packet contains one Station ID and a full `T x F x P` payload for the
configured frequency segment. Packets belong to the same group when this key
matches:

```text
(reference_epoch,
 seconds_from_reference_epoch,
 frame_number_within_second,
 first_channel_id,
 nchan,
 npol)
```

The key is compared lexicographically. The implementation does not derive a
continuous group number and does not require an integer number of groups per
second. `reference_epoch` must remain constant during a transfer.

Each configured Station ID must occur at most once in a group. The ordered
`antenna_map` defines its output A index and must contain exactly `NANT` unique
Station IDs.

## 4. Raw block and window geometry

For:

```text
H = 32                                      packet header bytes
P = packet payload bytes
R = raw records per ring block
A = NANT
B = configured window_blocks, default 2
```

the following invariants apply:

```text
raw_record_bytes             = H + P
raw_block_bytes              = R * raw_record_bytes
nominal_groups_per_raw_block = R / A
group_bytes                  = A * P
window_capacity_groups       = B * nominal_groups_per_raw_block
window_bytes                 = B * R * P
```

`R` must be divisible by `A`, and `B` must be at least 2. `window_bytes` is
payload-only; the 32-byte packet headers are not copied into the window. All
arithmetic is overflow checked and the derived allocation must not exceed the
configured memory limit.

`window_blocks=2` retains one previous raw block while ingesting the current
raw block. It therefore supports Station packet reordering across one raw-ring
block boundary. Larger observed reordering requires a larger configured value.

## 5. Reassembly window memory

The unpack process owns exactly one fixed-size reassembly arena in ordinary
host memory. PSRDADA owns the raw and compute ring blocks; neither ring block is
counted as unpack private memory.

Each active slot contains:

```text
GroupSlot
  key
  first_seen_raw_block_sequence
  present_bitmap[A]
  received_station_count
  tfpa_payload[T,F,P,A]
```

Slot payload memory is zeroed when allocated. For every accepted raw packet the
worker parses and validates the 32-byte header, resolves the Station ID through
`antenna_map`, and scatters:

```text
packet.payload[t,f,p]
  -> slot.tfpa_payload[t,f,p,antenna_map[station_id]]
```

The window never retains a pointer into a raw ring block. After all records in
an input block have either been copied into a slot or rejected, the raw block
lease can be released safely.

## 6. Window advancement and loss policy

Active slots are ordered by `GroupKey`. After raw block sequence `n` has been
ingested, the worker repeatedly examines the oldest slot. A slot is stable when

```text
n - first_seen_raw_block_sequence >= window_blocks - 1
```

Only a stable ordered prefix is emitted. If the oldest slot is not stable, no
later slot can overtake it. With the default `window_blocks=2`, a group first
seen in block `n` remains open through all of block `n+1` and is eligible for
emission after that block has been ingested.

When an incomplete group leaves the window, the zero-initialized slices for
missing Station IDs are retained and the group is emitted. The transfer and
process continue. At end-of-data, all remaining active groups are finalized in
key order using the same zero-fill policy.

Packets for a key older than the last emitted key are late packets. They are
dropped and counted. If the number of active groups exceeds the fixed arena
capacity, the oldest group is finalized with zero fill, the event is counted as
a window eviction, and its slot is reused.

The first version can detect missing Station packets only for a group represented
by at least one valid packet. A group for which every Station packet is absent
has no observable key and cannot be synthesized without an additional sequence
or cadence contract. This is an explicit first-version limitation.

## 7. Compute ring output

The compute ring contains payload only:

```text
order = TFPA
shape per group = [packet_nsamp, NCHAN, NPOL, NANT]
sample format = CI8 or CI16
record header bytes = 0
```

The output writer holds the current PSRDADA compute block and appends finalized
groups in key order. A group already has TFPA layout in the reassembly slot, so
each sample is copied to its final compute-ring position exactly once.

The compute block capacity must be an integer multiple of `group_bytes`. The
recommended geometry is one or more nominal unpacked raw-block payloads:

```text
compute_block_bytes = K * R * P
```

When the current compute block is full it is committed and the next block is
opened. At EOD, a non-empty final partial block is committed with its actual
byte count; that byte count must still be divisible by `group_bytes`. No extra
time groups are invented merely to fill the final block.

## 8. Header propagation

At transfer open, `vdif_unpack_worker` reads the raw ring's PSRDADA ASCII header,
cross-checks it against the unpack JSON and packet-format profile, constructs
the compute header, and publishes that header before output data.

All unknown upstream observation fields are preserved. The unpack transform
updates at least:

```text
DATA_STAGE=UNPACKED
ORDER=TFPA
RECORD_HEADER_BYTES=0
RECORD_BYTES=NCHAN*NPOL*NANT*complex_sample_bytes
RESOLUTION=RECORD_BYTES
MEMORY=HOST or PINNED_HOST
SAMPLE_FORMAT=CI8 or CI16
MISSING_PACKET_POLICY=ZERO_FILL
REORDER_WINDOW_BLOCKS=<configured value>
```

`UTC_START`, `MJD_START`, observation identity, `NANT`, `NCHAN` and `NPOL` are
preserved after validation. `PKT_HEADER=32`, `PKT_DATA`, `PKT_NSAMP` and
`PKT_TSAMP` remain as source-packet provenance even though compute data records
do not contain packet headers. Nominal byte-rate and file/block geometry fields
are recomputed for payload-only TFPA data.

For a continuous transfer whose final group count is not known when the output
header is published, the worker writes `TRANSFER_SIZE=0`. Per-transfer loss
totals cannot be inserted into the already-published header and are reported
through logs and the final statistics summary instead.

## 9. Error handling and statistics

The following data-quality errors do not terminate the process or transfer:

- unknown Station ID;
- duplicate Station packet in one group;
- invalid Project VDIF field or inconsistent packet geometry;
- invalid-data flag;
- late packet;
- receive frame length different from the configured fixed wire length;
- incomplete group leaving the window.

The offending packet is discarded, its receive work request is reposted when
applicable, and counters are updated. Hardware CQ failures, invalid WR IDs,
ring lifecycle failures, allocation failures and violated internal invariants
terminate the process with a non-zero status because safe continuation is not
guaranteed.

Counters use 64-bit integers and include:

```text
received_frames
accepted_packets
wrong_length_frames
invalid_header_packets
invalid_data_packets
unknown_station_packets
duplicate_packets
late_packets
window_evictions
completed_groups
incomplete_groups
missing_station_packets
expected_station_packets_for_observed_groups
```

The reported loss ratio is:

```text
missing_station_packets / expected_station_packets_for_observed_groups
```

It is explicitly labeled as applying to observed groups because completely
unobserved groups are not measurable in this version. Detailed packet logs are
rate limited; counters, periodic summaries and the final transfer summary are
not suppressed.

## 10. UDP FPGA simulator

The simulator uses an ordinary UDP socket and a JSON configuration. One process
has one Station ID. A shared observation section supplies the destination,
Project VDIF geometry, reference epoch, initial second, and deterministic
payload pattern. Functional simulations begin with frame zero at that second.

All senders derive header time from the same zero-based group index. The sample
interval in microseconds must convert exactly to a positive integer number of
picoseconds; otherwise simulator configuration is rejected before
transmission:

```text
packet_duration_ps = packet_nsamp * sample_interval_ps
group_start_ps      = group_index * packet_duration_ps
```

The integer accumulator selects `seconds_from_reference_epoch` and resets the
per-second frame ordinal whenever the integer second changes. This does not
require an integer number of groups per second and avoids independent
floating-point clock drift between sender hosts.

The simulator supports:

- realtime rate control using packet duration;
- unpaced burst mode;
- a future common start time for multi-server synchronization;
- a finite group count for repeatable tests;
- deterministic CI8 and CI16 sample generation;
- opt-in injection of Station packet loss, duplicates, local reordering and
  invalid header fields.

The first version does not provide a raw-verbs or line-rate backend.

## 11. Verification

Portable tests run on macOS without PSRDADA, libibverbs or CUDA:

- exact 32-byte header encode/decode golden vectors;
- CI8 and CI16 payload geometry validation;
- arbitrary Station arrival order producing exact TFPA output;
- one group crossing two synthetic raw blocks;
- duplicate, unknown, bad, late and invalid-data packet handling;
- incomplete group zero fill and observed-group loss ratio;
- active-window overflow and slot reuse;
- output ordering and full/partial compute block assembly;
- raw-to-compute header transform, including preservation of unknown fields;
- UDP simulator datagram capture and deterministic payload verification.

Linux integration tests then cover destination-only flow steering, wrong-length
frame discard with WR repost, raw-ring lifecycle, compute-ring lifecycle and
multi-server UDP transmission to one receiver. The expected compute data is
generated independently from sender formulas rather than by reusing the unpack
implementation.

## 12. Non-goals

- FPGA line-rate or raw-verbs sender performance;
- GPU unpack, conversion or algorithm execution;
- multiple frequency segments in one unpack worker;
- reconstruction of groups for which all Station packets are absent;
- VLAN, IPv6, IP options or fragmented UDP datagrams;
- dynamic observation geometry within one transfer.
