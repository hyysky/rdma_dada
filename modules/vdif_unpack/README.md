# vdif_unpack

Host-side unpack boundary that validates raw Project VDIF records, removes the
fixed 32-byte header, aggregates Station IDs, and converts per-Station TFP
payloads into payload-only, block-scoped ATFP compute blocks. The wire contract
is defined by [`doc/PROJECT_VDIF_PROFILE_V1.md`](../../doc/PROJECT_VDIF_PROFILE_V1.md)
and [`config/packet_formats/README.md`](../../config/packet_formats/README.md).

## Contract

- input record: `32-byte Project VDIF v1 header + TFP/IQ payload`;
- output: `UNPACKED/ATFP/CI8`, physical block order `[A,T,F,P]`;
- Station ID maps to the configured A-axis index;
- `GROUP_PERIOD_PS` plus observation start defines the strict group ordinal;
- `EXPECTED_GROUPS` is the exclusive finite-transfer boundary;
- missing data policy is `LOSS_POLICY=ZERO_FILL`;
- packet headers never enter the private window or compute ring.

The engine preserves unknown observation metadata while compiling the compute
header. It rejects inconsistent geometry, capacity, Station mapping, timeline,
order or sample format before publishing data.

## Parallel engine

One process contains a coordinator, a fixed parser worker pool and a sole
compute writer. Each raw block follows two parallel phases separated by
coordinator-owned ordering decisions:

1. workers decode contiguous record ranges into private preallocated
   `ParsedRecordDescriptor` arrays;
2. the coordinator visits descriptors in original record order, updates
   watermarks, resolves duplicates and reserves unique destinations;
3. workers copy accepted 4096-byte payloads into distinct window slots;
4. the coordinator finalizes ordered window leases;
5. the writer alone acquires, fills and commits compute-ring blocks.

`ParsedRecordDescriptor` is a 16-byte POD containing ordinal, record index,
antenna and flags; it owns no pointer or heap allocation. Source addresses are
reconstructed from the current raw-block base and record index. Queues,
descriptor arrays and window lease state are bounded and allocated during
preparation.

Window ranges follow `FREE -> FILLING -> FINALIZED -> WRITING -> FREE`.
Coordinator reuse is ACK-controlled, so a range held by the writer cannot be
overwritten. The writer commits strictly increasing ordinal ranges, handles
circular wrap, and submits only valid bytes for the final partial block.

## Ordering, loss and statistics

Watermark decisions use all configured Stations. One leading Station cannot
evict data that a lagging Station may still deliver; a Station process that
fails to participate aborts the observation at the controller level. Within a
live observation, late/missing/duplicate/invalid/unknown/out-of-range records
follow the configured count, drop and zero-fill policy.

Statistics cover received/accepted records, decode rejects, duplicates, late
and missing data, complete/incomplete/fully-missing groups, per-Station
ordinals, queue high-water marks, parse/copy/writer waits and total raw-block
service time. No per-packet logging occurs in the measured path.

CPU correctness uses deterministic `SyntheticVdifSource` arrival profiles and
independent byte-level golden data. PSRDADA lifecycle and affinity are composed
by `apps/vdif_unpack_worker`; the direct receiver boundary and current rate
evidence are summarized in
[`../../docs/VDIF_UNPACK_STATUS.md`](../../docs/VDIF_UNPACK_STATUS.md).
