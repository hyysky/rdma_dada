# RDMA staged-copy ablation

## Objective

Provide a reproducible receive-only reference path for the Section 3 placement
ablation without weakening or replacing the accepted production receiver.

The comparison boundary is fixed to:

`senders -> rdma2dada{,_staged_copy} -> raw ring -> dada_dbnull`

The matched variable is payload placement only:

- `NSGE2_DIRECT`: the NIC scatters the 42-byte Ethernet/IP/UDP prefix to a
  scratch buffer and the Project VDIF record directly into a raw-ring slot.
- `STAGED_COPY`: the NIC receives the complete frame into a registered staging
  slot and the same CQ/poll thread copies the Project VDIF record into the raw
  ring before reposting the WR.

Both paths retain one destination flow, one QP, one CQ, one poll/copy thread,
the same WR depth, poll batch, CPU/NUMA placement, ring geometry, consumer,
drain rule and sender topology. The historical multi-thread/multi-shard copy
implementation is not reused because it changes more than placement.

## Product isolation

- Keep `rdma2dada` and its `NSGE2_DIRECT` behavior unchanged.
- Add a separate executable `rdma2dada_staged_copy`.
- Permit `STAGED_COPY` only for the versioned receive-only topology.
- Require an explicit controller argument/experiment; the baseline default is
  always `NSGE2_DIRECT`.
- Record placement mode and selected receiver binary identity in preflight,
  run and compact result JSON.

## Receiver design

`StagedCopyReceiver` owns one registered frame buffer containing one slot per
receive WR. Every WR has one SGE (`recv_nsge=1`). On a valid completion the
poll thread copies bytes `[42, 42 + raw_record_bytes)` into the next raw-ring
record slot, advances block progress, commits full blocks in order and reposts
the completed WR in the same batch. Wrong-length packets retain the production
zero-fill/error threshold. Stop uses the production one-second drain rule.

The PSRDADA adapter gains a host-write lease operation with the same ordered
two-block ownership contract as `AcquireWriteBlock`, but without requiring an
ibverbs MR for raw-ring blocks.

## Comparison evidence

Both paths publish the existing exact packet/block/CQ/repost/drain counters and
the following matched evidence:

- placement mode and staged-copy bytes;
- receive-thread CPU time and CPU-seconds per published GB;
- receive/publication batch latency P50/P95 (fixed low-overhead histogram);
- raw-ring used-byte high-water mark and block-acquire wait total/max;
- packet deficit, wrong-length/CQ/repost errors and cleanup state.

Instrumentation is sampled once per non-empty CQ batch or ring-block event,
not once per packet.

## Acceptance sequence

1. Portable policy/controller tests, including fail-closed topology selection.
2. GPU-server clean build and receiver preflight for both binaries.
3. One low-rate functional run per placement.
4. Matched receive-only approximately 30 Gb/s, 60 s, warm-up plus three
   measured repetitions per placement, fixed baseline ports/CPU/NUMA/rings.
5. Import compact suites into the result catalog. A staged-copy performance
   failure is retained as the ablation boundary rather than discarded.

