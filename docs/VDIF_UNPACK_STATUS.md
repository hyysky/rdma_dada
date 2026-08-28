# VDIF ingest and ATFP unpack status

Updated: 2026-08-28
Committed baseline: `058f4bc`

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

### Accepted repeatable evidence

- module/reference, partial/EOD, rollover, duplicate, missing and zero-fill
  fixtures pass;
- PSRDADA raw→compute integration and thread-affinity paths pass;
- receive-only 30 Gbps/60 s completed one warm-up plus three measured runs with
  exact sender/receiver closure, fixed source ports, clean dbnull/EOD and
  cleanup; result root:
  `/home/user/wy/task8c-drain-results/drain-receive-30Gbps-60s-20260826-r2`;
- receive+unpack 30 Gbps/60 s completed the same repetition contract with
  exact sender→receiver→unpack closure and zero missing/late/duplicate/header
  errors; result root:
  `/home/user/wy/task8c-unpack-results/unpack-30Gbps-60s-20260826-r3`;
- the accepted unpack profile fixes receiver CPU 13, coordinator/parser/writer
  CPUs 14--19, sink CPU 20, NUMA1, WR depth 1024, poll batch 32, source ports,
  ring/window geometry and one-second preparation.

### Remaining limits

- 35 Gbps/60 s failed with receiver delivery far below the planned record
  count; available evidence does not identify unpack as the first saturated
  stage;
- all current rate results stop at `dada_dbnull` on the compute ring and exclude
  `pipeline_worker`, H2D, GPU algorithms, D2H and output ring.

The accepted wording is therefore:

> 30 Gbps is the accepted repeatable payload rate for the receive-only and
> receive+unpack boundaries on qths1. The full GPU-pipeline stable rate has not
> been established.

## Reuse rule

Later GPU/full tests inherit the accepted profile rather than silently changing
CPU/NUMA, source ports, preparation or ring/window geometry. They do not rerun
the receive/unpack acceptance unless the implementation, hardware boundary or
profile changes. Complete GPU-pipeline acceptance remains a separate gate.

The next executable work is defined in
[`superpowers/plans/2026-08-19-receiver-admission-and-full-pipeline-acceptance.md`](superpowers/plans/2026-08-19-receiver-admission-and-full-pipeline-acceptance.md).
