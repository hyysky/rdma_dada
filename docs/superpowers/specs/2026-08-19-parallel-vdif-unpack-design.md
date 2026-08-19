# Parallel VDIF Unpack Design

## Goal

Sustain the existing 30 Gbps two-Station stream through
`raw ring -> VDIF unpack -> compute ring -> dada_dbnull` by parallelizing
VDIF parsing and payload placement while preserving one PSRDADA raw reader and
one PSRDADA compute writer.

## Scope

- Keep the direct NSGE=2 RDMA receiver unchanged.
- Keep one `vdif_unpack_worker` process.
- Use one coordinator thread, one fixed worker pool, and one writer thread.
- Keep exactly one compute-ring writer.
- Do not add intermediate PSRDADA rings or worker processes.
- Preserve byte-for-byte ATFP output, timeline, missing-data, duplicate, EOD,
  and final-partial semantics.

## Thread placement

`vdif_unpack_worker` requires an explicit ordered CPU list:

```text
--thread-cpus COORDINATOR,WORKER...,WRITER
```

The list must contain at least three distinct CPUs. The worker count is derived
as `cpu_count - 2`; there is no second thread-count setting. The application
pins every pthread and verifies the effective affinity. The Task8C profile uses
NUMA1 cores as follows:

```text
RDMA poll: 13
coordinator: 14
workers: 15,16,17,18
writer: 19
sink: 20
```

CPU numbers are profile values, not product constants.

## Compact descriptor

Each parsed record uses a fixed-size POD descriptor with no pointer, string, or
heap ownership:

```cpp
struct ParsedRecordDescriptor {
    std::uint64_t ordinal;
    std::uint32_t record_index;
    std::uint16_t antenna;
    std::uint8_t flags;
    std::uint8_t reserved;
};
```

The descriptor is 16 bytes. `record_index` reconstructs the source address from
the current raw-block base. Flags distinguish decoded, invalid-data, accepted,
duplicate, late, and rejected records. Each worker owns a preallocated
descriptor array sized for its contiguous raw-block range. Runtime processing
performs no descriptor allocation.

## Per-block pipeline

1. The coordinator receives one PSRDADA raw block and divides its records into
   contiguous ranges.
2. Workers parse their ranges concurrently into private descriptor arrays and
   private statistics.
3. After a parse barrier, the coordinator visits descriptors in original raw
   record order. It updates station watermarks and statistics, reserves window
   slots, resolves duplicates deterministically, and marks accepted descriptors.
4. Workers copy accepted 4096-byte payloads concurrently from the still-owned
   raw block into distinct window slots.
5. After a copy barrier, the coordinator finalizes eligible compute-block
   ranges and puts immutable window leases into a bounded SPSC ready queue.
6. `ProcessBlock` may return only after all workers have stopped reading the raw
   block. The writer may continue copying an older finalized window range while
   workers process the next raw block.

## Window ownership

Each compute-block-sized logical window range follows:

```text
FREE -> FILLING -> FINALIZED -> WRITING -> FREE
```

- Workers only write `FILLING` ranges.
- A range becomes `FINALIZED` only after its payload-copy barrier and missing
  payload zero-fill complete.
- The writer consumes ready leases in increasing `first_group_ordinal` order.
- The coordinator cannot clear or reuse a range until the writer returns its
  token on the ACK SPSC queue.
- If the circular window would wrap onto an unacknowledged range, the
  coordinator waits for the writer instead of overwriting it.

Both queues and all lease records are allocated during preparation. Queue
capacity is derived from `window_blocks`; no unbounded queue is permitted.

## Writer

The writer is the only thread allowed to call compute-ring Acquire/Commit. For
each ready lease it:

1. acquires one compute block;
2. copies the antenna-major finalized view, including circular wrap handling;
3. commits the exact valid byte count;
4. returns the lease token to the coordinator.

No second output ring and no multi-writer PSRDADA access are introduced.

## Failure and EOD

- Any worker, coordinator, writer, ring, or affinity error sets a shared fatal
  state and stops the complete transfer.
- On EOD, the coordinator finishes current worker barriers, finalizes the last
  partial range, waits for all writer ACKs, calls writer `Finish`, then joins all
  threads.
- A writer failure aborts the sink and wakes coordinator/worker waits.
- Statistics and errors are emitted once after thread shutdown; no per-packet
  logging is allowed.

## Measurements

Record cumulative and maximum values for parse, coordinator, payload-copy,
writer acquire wait, writer memcpy, writer commit, ready-queue wait, ACK wait,
and total raw-block service time. Record queue high-water marks and verified
thread affinities.

At 30 Gbps a 52,838,400-byte raw block arrives about every 14 ms. Target both
worker-side and writer-side p99 service below 11 ms, with no sustained raw-ring
full condition and exact formal packet/group closure.

## Acceptance

- One-thread-pool and four-worker outputs match the existing reference fixtures
  byte-for-byte for interleaved, 425/2112 Station burst, duplicates, invalid
  data, missing packets, wrap, rollover, and final partial cases.
- No data race permits a writer-held window range to be reused.
- Writer commits remain strictly ordered.
- Startup failure, worker failure, writer failure, and EOD leave no ring/process
  residue.
- qths small correctness fixture passes three consecutive repetitions.
- Performance runs proceed 15 Gbps then 30 Gbps; every formal record/group must
  close with zero missing/late/invalid/duplicate output errors.
