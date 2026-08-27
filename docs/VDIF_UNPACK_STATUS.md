# VDIF ingest and ATFP unpack status

Updated: 2026-08-20
Code baseline: `b3f8d64`

This document records the current product boundary and performance evidence for
`rdma2dada -> raw ring -> vdif_unpack_worker -> compute ring`. It deliberately
does not treat sender rate or a one-off successful run as formal acceptance.

## Product architecture

### Direct raw-ring receiver

`rdma2dada` uses one destination-only flow, one QP/CQ and one poll thread. Each
receive WR has two SGEs:

- 42-byte Ethernet/IPv4/UDP header -> fixed registered scratch;
- Project VDIF record -> the assigned PSRDADA raw-ring record slot.

The receiver holds two raw blocks concurrently, tracks completion per slot,
publishes blocks in order, reposts WRs, and commits an exact partial tail at
EOD. The normal path has no packet-to-ring payload memcpy. Current test profile
defaults are WR depth 1024 and CQ poll batch 32; these are configurable rather
than protocol constants.

### Parallel ATFP unpack

One `vdif_unpack_worker` process keeps one raw-ring reader and one compute-ring
writer. Its threads are:

- coordinator: owns raw-block lifecycle, descriptor order, watermarks and
  window reservation;
- parser workers: decode contiguous record ranges and copy accepted payloads;
- writer: exclusively owns compute-ring Acquire/Commit and ordered publication.

The worker uses 16-byte pointer-free descriptors, preallocated bounded queues,
and ACK-controlled window leases. It outputs payload-only, block-scoped
`[A,T,F,P]` data and preserves missing/duplicate/late/invalid/EOD semantics.

The current qths1 NUMA1 test profile is receiver CPU 13, coordinator 14,
workers 15–18, writer 19 and sink 20. These values belong to the test profile,
not the product contract.

## Measured geometry

The latest campaign uses:

- Project VDIF record: 4,128 bytes = 32-byte header + 4,096-byte payload;
- raw block: 52,838,400 bytes = 12,800 records;
- compute block: 52,428,800 bytes;
- block arrival interval at 30 Gbps payload: approximately 14 ms.

Window blocks and reorder horizon are derived from requested rate,
`missing_wait_ms`, Station-skew reserve and block geometry. Ring/window capacity
is burst tolerance; it is not evidence of sufficient steady-state service rate.

## Current evidence

### Passed development evidence

- module/reference, partial/EOD, rollover, duplicate, missing and zero-fill
  fixtures pass;
- PSRDADA raw→compute integration and thread-affinity paths pass;
- 30 Gbps unpack-only 30-second and 60-second exploratory runs have achieved
  exact sender→receiver→unpack closure with clean dbnull/EOD;
- representative 60-second result:
  `/home/user/wy/task8c-unpack-results/unpack-30Gbps-60s-20260819T031851Z`.

### Not yet accepted

- two 30 Gbps repeatability suites observed small receiver/NIC admission
  deficits of 251 and 700 packets; unpack processed all records delivered by
  the receiver, so those suites did not pass the formal gate;
- 35 Gbps/60 s failed with receiver delivery far below the planned record
  count; available evidence does not identify unpack as the first saturated
  stage;
- all current rate results stop at `dada_dbnull` on the compute ring and exclude
  `pipeline_worker`, H2D, GPU algorithms, D2H and output ring.

The accepted wording is therefore:

> 30 Gbps is the highest observed single-run exact-closure payload rate for the
> unpack-only boundary. A repeatable stable rate and the full GPU-pipeline rate
> have not yet been established.

## Current project decision

On 2026-08-20 the project chose to use 30 Gbps payload as a provisional
receive/unpack planning baseline and proceed to full GPU-pipeline work. This
does not rewrite the evidence above: the repeatable warm-up-plus-three gate is
deferred, not passed, and must be restored before publishing a formal stable
ingest/unpack rate.

## Formal acceptance still required

1. Receive-only at 30 Gbps: one warm-up plus three 60-second measured runs,
   exact sender/receiver closure, clean EOD and cleanup.
2. Unpack-only at 30 Gbps: same repetitions, with receiver/unpack/group/byte
   reconciliation and no additional unpack loss.
3. Every run must preserve NIC before/after deltas, CQ service counters,
   accepted/published counts, raw/compute occupancy, per-stage service time,
   affinity/NUMA evidence and machine-readable result artifacts.
4. After those gates, run the complete GPU pipeline separately; unpack-only
   results cannot be reused as full-pipeline acceptance.

The next executable work is defined in
[`superpowers/plans/2026-08-19-receiver-admission-and-full-pipeline-acceptance.md`](superpowers/plans/2026-08-19-receiver-admission-and-full-pipeline-acceptance.md).
