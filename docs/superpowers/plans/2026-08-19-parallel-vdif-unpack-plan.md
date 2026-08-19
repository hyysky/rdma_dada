# Parallel VDIF Unpack Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a fixed-affinity worker pool and concurrent single writer so VDIF unpack can sustain the 30 Gbps direct receive stream without changing ATFP output.

**Architecture:** One coordinator owns PSRDADA raw reads and window metadata, multiple fixed-core workers parse and copy record batches, and one fixed-core writer owns all compute-ring writes. Finalized window ranges remain leased until writer acknowledgement.

**Tech Stack:** C++11, pthreads, PSRDADA, bounded preallocated queues, CMake/CTest, Python Task8C controller.

**Spec:** `docs/superpowers/specs/2026-08-19-parallel-vdif-unpack-design.md`

## Global Constraints

- Do not modify the direct NSGE=2 RDMA receiver.
- Do not create extra PSRDADA rings or unpack processes.
- Exactly one thread owns compute-ring Acquire/Commit.
- Descriptor storage is preallocated and contains no pointers or strings.
- CPU IDs come from an explicit ordered list and are never hard-coded.
- No per-packet logging or allocation in the measured path.
- Preserve all existing timeline, ordering, missing-data, EOD, and cleanup semantics.
- Do not commit or push without explicit user authorization.

---

### Task 1: Lock the compact descriptor and affinity contract

**Files:**
- Modify: `include/rdma_dada/modules/vdif_unpack/vdif_atfp_engine.h`
- Modify: `apps/vdif_unpack_worker/main.cpp`
- Modify: `tests/vdif_atfp_engine_test.cpp`
- Modify: `tests/vdif_unpack_config_test.cpp`

**Interfaces:**
- Produces: `ParsedRecordDescriptor` with an asserted size of 16 bytes.
- Produces: an ordered CPU-list parser whose first CPU is coordinator, last is writer, and middle CPUs are workers.

- [ ] Add failing tests for descriptor size, flag encoding, source-address reconstruction, minimum three distinct CPUs, malformed lists, duplicates, and out-of-range CPU values.
- [ ] Run the focused tests and confirm they fail for missing interfaces.
- [ ] Add the 16-byte descriptor and strict ordered CPU-list parser.
- [ ] Pin and verify coordinator affinity; add helpers used later for worker/writer pthread affinity.
- [ ] Run focused tests and the existing unpack configuration/header tests.

### Task 2: Add preallocated parse workers

**Files:**
- Modify: `include/rdma_dada/modules/vdif_unpack/vdif_atfp_engine.h`
- Modify: `modules/vdif_unpack/vdif_atfp_engine.cpp`
- Modify: `tests/vdif_atfp_engine_test.cpp`

**Interfaces:**
- Consumes: ordered worker CPUs and `ParsedRecordDescriptor`.
- Produces: parallel parse phase with per-worker descriptor arrays and statistics.

- [ ] Add failing tests comparing descriptor streams from one and four workers for valid, invalid-header, invalid-data, unknown-Station, out-of-range, rollover, and pre-timeline records.
- [ ] Add a deterministic test proving concatenated contiguous worker ranges preserve raw record order.
- [ ] Prepare worker pthreads, barriers, descriptor capacities, and private counters during engine preparation.
- [ ] Implement parallel header decode into worker-private arrays without heap allocation in `ConsumeRawBlock`.
- [ ] Merge private counters and descriptors in coordinator order.
- [ ] Run `vdif_atfp_engine_test` three consecutive times.

### Task 3: Parallelize payload placement safely

**Files:**
- Modify: `modules/vdif_unpack/vdif_atfp_engine.cpp`
- Modify: `tests/vdif_atfp_engine_test.cpp`

**Interfaces:**
- Consumes: parsed descriptors in raw record order.
- Produces: accepted descriptors with unique reserved window destinations and a parallel payload-copy phase.

- [ ] Add failing byte-for-byte tests for INTERLEAVED, STATION_BURST_425, STATION_BURST_2112, duplicates split across worker ranges, missing records, invalid data, and window wrap.
- [ ] Move duplicate resolution, slot activation, watermarks, and statistics into the coordinator reservation pass.
- [ ] Reuse descriptor flags to identify accepted copy jobs; do not allocate a second per-record object.
- [ ] Implement the worker copy phase and barrier; reconstruct source pointers from `record_index` and raw-block base.
- [ ] Verify workers only write distinct window destinations and raw blocks are not released before the copy barrier.
- [ ] Run reference-output comparisons and all unpack engine tests three times.

### Task 4: Lease finalized window ranges to one concurrent writer

**Files:**
- Modify: `include/rdma_dada/modules/vdif_unpack/vdif_atfp_engine.h`
- Modify: `modules/vdif_unpack/vdif_atfp_engine.cpp`
- Modify: `include/rdma_dada/modules/vdif_unpack/atfp_block_writer.h`
- Modify: `modules/vdif_unpack/atfp_block_writer.cpp`
- Modify: `apps/vdif_unpack_worker/main.cpp`
- Modify: `tests/atfp_block_writer_test.cpp`
- Modify: `tests/vdif_atfp_engine_test.cpp`

**Interfaces:**
- Produces: bounded SPSC ready and ACK queues.
- Produces: window lease tokens with `first_group_ordinal`, slot, group count, and state.
- Consumes: existing `AtfpBlockView` and `WritableBlockSink`.

- [ ] Add failing tests for `FREE -> FILLING -> FINALIZED -> WRITING -> FREE`, ordered commit, wrap protection, writer blocked while next raw block is processed, and ACK-controlled reuse.
- [ ] Add preallocated lease/state storage sized from `window_blocks`.
- [ ] Change finalization to zero-fill and enqueue an immutable lease without clearing its slots.
- [ ] Add the writer pthread; keep all sink Acquire/Commit calls on this thread.
- [ ] Return ACK only after successful commit; coordinator clears slots only after ACK.
- [ ] Block only when circular reuse reaches an unacknowledged lease or the bounded ready queue is full.
- [ ] Add writer acquire/memcpy/commit and queue-wait statistics using `CLOCK_MONOTONIC_RAW`.
- [ ] Run writer and engine tests three consecutive times.

### Task 5: Make failure, EOD, and teardown deterministic

**Files:**
- Modify: `apps/vdif_unpack_worker/main.cpp`
- Modify: `modules/vdif_unpack/vdif_atfp_engine.cpp`
- Modify: `tests/vdif_atfp_engine_test.cpp`
- Modify: `tests/atfp_block_writer_test.cpp`

**Interfaces:**
- Consumes: shared fatal state, worker barriers, ready/ACK queues.
- Produces: one shutdown sequence that joins all threads and closes the sink once.

- [ ] Add failing tests for worker startup failure, affinity failure, parse failure, writer Acquire failure, writer Commit failure, interrupted waits, final partial block, and normal EOD.
- [ ] Implement fatal-state publication and wake all waits on failure.
- [ ] Implement EOD order: finish workers, finalize partial, drain writer ACKs, writer Finish, join threads, close output transfer.
- [ ] Verify no commit occurs after failure and no thread accesses released raw/window memory.
- [ ] Run all focused tests three consecutive times.

### Task 6: Render configuration and preserve diagnostics

**Files:**
- Modify: `scripts/task8c_rate_point.py`
- Modify: `tests/task8c_rate_point_test.py`
- Modify: `apps/vdif_unpack_worker/README.md`
- Modify: `docs/agents/testing.md`
- Modify: `config/testing/atfp-throughput-source-manifest.sha256`

**Interfaces:**
- Produces: `--thread-cpus 14,15,16,17,18,19` in worker argv.
- Produces: result fields for effective affinities, thread count, per-stage service times, and queue high-water marks.

- [ ] Add failing controller tests for required CPU ordering, overlap with RDMA/sink cores, wrong list length, worker argv rendering, affinity artifact parsing, and missing diagnostics.
- [ ] Add runner/profile rendering for the explicit thread list and NUMA1 validation.
- [ ] Persist stage timings and actual pthread affinity in `result.json`.
- [ ] Update concise operational documentation and regenerate the complete source manifest.
- [ ] Run the controller test three times and `git diff --check`.

### Task 7: Linux correctness and performance gates

**Files:**
- No product edits in the server testing task.

**Interfaces:**
- Consumes: synchronized manifest, fresh qths Release build, fixed CPU mapping.
- Produces: server correctness and exploratory performance evidence.

- [ ] Notify `GPU服务器代码测试` with branch, manifest, exact files, build targets, CPU mapping, and acceptance criteria.
- [ ] On qths, run Phase0, full mirror verification, a unique fresh Release build, direct/CTest suites three times, and cleanup checks.
- [ ] Run the small PSRDADA correctness fixture three times and compare compute header plus valid payload byte-for-byte with the independent expected output.
- [ ] Run exactly one 15 Gbps exploratory point; require exact formal closure, clean EOD, and no sustained ring full.
- [ ] Only after 15 Gbps passes, run exactly one 30 Gbps exploratory point with the same hard gates.
- [ ] Report worker/writer p99 service against the 11 ms target, ring occupancy, receiver closure, and cleanup independently.
