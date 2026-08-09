# ATFP Unpack and GPU Transpose Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace CPU TFP-to-TFPA sample scatter with exact-timeline ATFP block assembly, then physically transpose and convert ATFP integer-complex samples to contiguous TFPA CF32 on the GPU before beamforming.

**Architecture:** `vdif_unpack_worker` maps `(reference_epoch, seconds, frame)` to a checked group ordinal, maps Station ID to the configured A index, and places each complete TFP payload into an antenna-major fixed circular window with one `memcpy`. Once a consecutive output range is final, a VDIF-specific writer acquires one compute-ring block, publishes compact block-scoped ATFP data, and commits it once. After H2D, `ComplexConvertModule` performs a fused ATFP-to-TFPA physical transpose, CI8/CI16 conversion, and scale application on the worker-owned CUDA stream.

**Tech Stack:** C++11, CUDA 12.8, CUDA Runtime, PSRDADA, CMake, Python 3 test controller, NVIDIA RTX 3090.

## Global Constraints

- The Project VDIF header remains exactly 32 bytes and is not extended.
- Network payload order remains TFP; the observation network format is CI8, `PKT_NBIT=16`, `TWOS_COMPLEMENT`, component order IQ, little-endian.
- Station identity comes only from the VDIF Station ID. `selection.antenna_map[A]` defines the A-axis and must match the A axis of FPAB beamforming weights.
- `COMPONENT_SIGNED` and `SOURCE_COMPONENT_SIGNED` are removed. CI8/CI16 plus `TWOS_COMPLEMENT` defines signed input.
- Group zero is supplied by the raw DADA header, starts at frame zero on the configured whole-second boundary, and the exclusive stop boundary is `EXPECTED_GROUPS`.
- Completely absent groups and missing Stations are zero-filled; an unavailable or failed configured sender aborts the complete observation at the controller level.
- The production unpack fast path does not use `std::map` and remains single-threaded for this version.
- The private payload window is `[A, circular_group, packet_T, F, P]`; metadata is a fixed group-slot array tagged with its owned ordinal.
- Only one compute-ring writable data block may be held. Each block is acquired once, filled completely, committed once, then released before another acquire.
- Each compute block is independently ordered ATFP. A partial EOD block uses its actual group count and contains no antenna-plane gaps.
- CUDA produces physically contiguous TFPA CF32; it does not expose ATFP through a stride-only view.
- `CONVERSION_SCALE` is positive, finite, explicitly configured, and applied exactly once.
- Existing Beamform, Power, Stokes and TimeIntegrate input/output contracts after converted TFPA remain unchanged.
- Local/macOS tests are development evidence only. Every completed behavior is handed to `GPU服务器代码测试`, and development waits for its callback before advancing.
- Test hosts do not use Git. Synchronization uses a development-supplied SHA256 manifest and absolute executable paths.
- Do not create a Git commit or push automatically. After server PASS, remind the user that the checkpoint is ready for their Git decision.
- Preserve unrelated dirty-worktree changes, especially TimeIntegrate benchmark/optimization files.

## Test and Handoff Protocol for Every Task

1. Write the failing test before product code.
2. Run the focused test and record the expected failure.
3. Implement only the task behavior.
4. Run the focused test, adjacent regressions, and three consecutive clean repetitions where the task is ready for acceptance.
5. Generate SHA256 values for every affected file.
6. Notify `GPU服务器代码测试` with the source task ID, affected files, exact build/test commands, fixtures, expected results, and cleanup requirements.
7. Wait for an explicit callback. Keep `TEST_RESULT`, `RESULT_NOTIFICATION`, and `CLEANUP_RESULT` independent.
8. On FAIL/HARNESS_FAIL/ENV_BLOCKED, diagnose from returned evidence, modify locally, and repeat the same handoff. Do not advance to the next task.

---

### Task 1: Exact Observation Timeline and Header Boundary

**Files:**
- Create: `include/rdma_dada/modules/vdif_unpack/vdif_timeline.h`
- Create: `modules/vdif_unpack/vdif_timeline.cpp`
- Create: `tests/vdif_timeline_test.cpp`
- Modify: `modules/vdif_unpack/vdif_unpack_header.cpp`
- Modify: `include/rdma_dada/modules/vdif_unpack/vdif_unpack_header.h`
- Modify: `CMakeLists.txt`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: raw DADA metadata fields `GROUP_PERIOD_PS`, `GROUP_START_REFERENCE_EPOCH`, `GROUP_START_SECONDS`, `GROUP_START_FRAME`, `EXPECTED_GROUPS`; Project VDIF header time fields.
- Produces:

```cpp
struct VdifTimeline {
    std::uint64_t group_period_ps;
    std::uint8_t start_reference_epoch;
    std::uint32_t start_seconds;
    std::uint32_t start_frame;
    std::uint64_t expected_groups;
};

bool ParseVdifTimeline(const pipeline::Metadata& header,
                       const PipelineConfig& pipeline,
                       VdifTimeline* timeline,
                       std::string* error);
bool VdifOrdinalToTime(const VdifTimeline& timeline,
                       std::uint64_t ordinal,
                       std::uint32_t* seconds,
                       std::uint32_t* frame,
                       std::string* error);
bool VdifTimeToOrdinal(const VdifTimeline& timeline,
                       std::uint8_t reference_epoch,
                       std::uint32_t seconds,
                       std::uint32_t frame,
                       std::uint64_t* ordinal,
                       std::string* error);
```

- [x] **Step 1: Add failing timeline tests**

  Cover ordinal zero, second rollover, non-integral groups per second, exact inverse mapping, mismatched epoch, before-start time, non-canonical frame, `ordinal == EXPECTED_GROUPS`, zero fields, `GROUP_START_FRAME != 0`, multiplication overflow, a `PKT_TSAMP` value that cannot be represented as integer picoseconds, and the required equality `GROUP_PERIOD_PS == PKT_NSAMP * integer_ps(PKT_TSAMP_US)`.

  The asymmetric non-integral case uses `GROUP_PERIOD_PS=600000000000`, producing `(seconds,frame)` values `(S,0)`, `(S,1)`, `(S+1,0)`, `(S+1,1)` for ordinals 0–3.

- [x] **Step 2: Register and run the failing test**

  ```bash
  cmake -S . -B build-atfp-local -DBUILD_TESTING=ON -DBUILD_RDMA_PIPELINE=OFF -DUSE_CUDA=OFF
  cmake --build build-atfp-local --target vdif_timeline_test
  ctest --test-dir build-atfp-local -R '^vdif_timeline_test$' --output-on-failure
  ```

  Expected before implementation: compile/link failure because the timeline API is absent.

- [x] **Step 3: Implement checked integer timeline conversion**

  Use `1'000'000'000'000` conceptually but C++11-compatible `UINT64_C(1000000000000)` in source. Implement checked multiply/add and ceiling division without floating point. `VdifTimeToOrdinal` computes:

  ```text
  first = ceil((seconds-start_seconds)*1e12 / group_period_ps)
  candidate = first + frame
  ```

  It then regenerates seconds/frame from `candidate` and requires an exact match.

- [x] **Step 4: Parse and preserve the timeline in output metadata**

  `ParseVdifTimeline` requires all five raw-header fields and validates their ranges. `BuildVdifUnpackOutputHeader` preserves the exact values for downstream traceability and computes `TRANSFER_SIZE = EXPECTED_GROUPS * NANT * packet_payload_bytes` with overflow checks.

- [x] **Step 5: Run focused and adjacent tests three times**

  ```bash
  cmake --build build-atfp-local --target vdif_timeline_test vdif_unpack_header_test
  ctest --test-dir build-atfp-local -R '^(vdif_timeline_test|vdif_unpack_header_test)$' --output-on-failure
  ctest --test-dir build-atfp-local -R '^(vdif_timeline_test|vdif_unpack_header_test)$' --output-on-failure
  ctest --test-dir build-atfp-local -R '^(vdif_timeline_test|vdif_unpack_header_test)$' --output-on-failure
  ```

- [x] **Step 6: Perform the Task 1 server handoff and wait for callback**

  Request a Linux Release build and the same two tests repeated three times. Acceptance requires exact rollover/inverse/overflow behavior and no server resource creation.

---

### Task 2: Wire-Only Packet Profile and ATFP Output Metadata

**Status:** Completed as one atomic Task 2–4 checkpoint; qths1 Release portable
and PSRDADA integration tests passed three consecutive rounds.

**Files:**
- Modify: `include/rdma_dada/config/packet_format_config.h`
- Modify: `src/config/packet_format_config.cpp`
- Modify: `config/packet_formats/packet-format-v1.schema.json`
- Modify: `config/packet_formats/frontend.example-v1.json`
- Modify: `config/packet_formats/README.md`
- Modify: `modules/vdif_unpack/vdif_unpack_config.cpp`
- Modify: `modules/vdif_unpack/vdif_unpack_header.cpp`
- Modify: `tests/packet_format_config_test.cpp`
- Modify: `tests/vdif_unpack_config_test.cpp`
- Modify: `tests/vdif_unpack_header_test.cpp`
- Modify: `modules/vdif_unpack/README.md`
- Modify: `config/README.md`

**Interfaces:**
- Consumes: Project VDIF wire profile containing only header definition, `packed_order=[T,F,P]`, axes, CI8/IQ/two's-complement fields.
- Produces: unpack output metadata `DATA_STAGE=UNPACKED`, `ORDER=ATFP`, `LAYOUT_SCOPE=BLOCK`, `SAMPLE_FORMAT=CI8`, `SAMPLE_ENCODING=TWOS_COMPLEMENT`, `COMPONENT_ORDER=IQ`, `COMPONENT_NBIT=8`, `SAMPLE_NBIT=16`, `ENDIAN=LITTLE`.

- [x] **Step 1: Add failing schema/config/header tests**

  Verify that `payload.output_order` is rejected as an unknown wire-profile field, that the example loads without it, and that unpack output is ATFP with `BLOCK_NTIME`, `OUTPUT_BLOCK_BYTES`, zero record-header bytes, and exact timeline/transfer fields. Verify absence of `COMPONENT_SIGNED` and `SOURCE_COMPONENT_SIGNED`.

- [x] **Step 2: Run the focused tests and confirm failure**

  ```bash
  cmake --build build-atfp-local --target packet_format_config_test vdif_unpack_config_test vdif_unpack_header_test
  ctest --test-dir build-atfp-local -R '^(packet_format_config_test|vdif_unpack_config_test|vdif_unpack_header_test)$' --output-on-failure
  ```

- [x] **Step 3: Remove output order from the wire schema and C++ model**

  Delete `PacketFormatConfig::output_order`; update strict JSON key validation, schema, example and documentation together. Keep `packed_order` and the Station lookup A-axis description.

- [x] **Step 4: Publish the ATFP block contract**

  Set `BLOCK_NTIME` to the nominal compute-block T. Set `RESOLUTION=NANT*NCHAN*NPOL*2`, `RECORD_BYTES=OUTPUT_BLOCK_BYTES`, and validate actual block T later from `actual_bytes/(NANT*NCHAN*NPOL*2)`.

- [x] **Step 5: Run the three focused tests three consecutive times**

- [x] **Step 6: Perform the Task 2 server handoff and wait for callback**

  Acceptance requires strict schema behavior, the updated example fixture, exact ATFP header bytes/fields, and no GPU/RDMA resources.

---

### Task 3: Fixed Circular ATFP Reorder Engine

**Status:** Correctness implementation completed and accepted with Task 2–4.
The standalone throughput comparison is intentionally retained for Task 9 so
it uses the same versioned performance runner and rate matrix.

**Files:**
- Create: `include/rdma_dada/modules/vdif_unpack/atfp_block_view.h`
- Modify: `include/rdma_dada/modules/vdif_unpack/vdif_unpack_engine.h`
- Modify: `modules/vdif_unpack/vdif_unpack_engine.cpp`
- Modify: `tests/vdif_unpack_engine_test.cpp`
- Modify: `modules/vdif_unpack/README.md`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: `VdifTimeline`, validated Project VDIF records, `antenna_map`, and layout geometry.
- Produces:

```cpp
struct AtfpBlockView {
    const std::uint8_t* window_data;
    std::uint64_t window_capacity_groups;
    std::uint64_t first_group_ordinal;
    std::uint64_t first_slot;
    std::uint64_t group_count;
    std::uint32_t nant;
    std::uint64_t packet_payload_bytes;
};

typedef std::function<bool(const AtfpBlockView&, std::string*)>
    VdifAtfpBlockEmitter;
```

  `VdifUnpackEngine::Configure` additionally consumes `const VdifTimeline&`.

- [x] **Step 1: Replace the old tests with a failing independent ATFP oracle**

  Use asymmetric `A=3`, `packet_T=5`, `F=2`, `P=2`; encode every payload byte from `(station, ordinal, t, f, p)`. Feed arbitrary Station order and compare emitted logical data with:

  ```text
  expected[(((a*G + g)*packet_T + t)*F + f)*P + p]
  ```

  Keep a test-only legacy TFPA scatter oracle and verify that ATFP followed by an independent CPU transpose is byte-identical.

- [x] **Step 2: Add failing timeline/window edge tests**

  Cover duplicate/unknown/invalid/late/out-of-range records, a missing Station, a fully absent leading/internal/trailing group, second rollover, group slot reuse, exact circular wrap, ordinal distance equal to capacity, a far-future packet, EOD, and two consecutive configured engine instances.

- [x] **Step 3: Replace dynamic maps with fixed storage**

  Allocate:

  ```text
  payload_window_bytes = NANT * window_capacity_groups * payload_bytes
  metadata_slots = window_capacity_groups
  station_lookup_entries = 65536
  ```

  Initialize Station lookup entries to an invalid sentinel. For accepted `(a,g)`, copy exactly one complete payload to `(a*capacity + g%capacity)*payload_bytes` and set the metadata bitmap.

- [x] **Step 4: Implement group-distance finalization**

  Set `groups_per_compute_block=compute_block_bytes/(NANT*payload_bytes)` and `reorder_horizon=capacity-groups_per_compute_block`. A group is final when complete or `highest_seen-group >= horizon`. Emit only consecutive ranges, advance/zero-fill full block ranges to admit a far-future valid packet, and flush through `EXPECTED_GROUPS-1` at EOD.

- [x] **Step 5: Zero only missing Station slices before synchronous emission**

  Treat an inactive/tag-mismatched slot as a fully missing group. Clear only missing `(a,g)` payload slices immediately before constructing the view. Do not clear the complete payload window on slot reuse.

- [x] **Step 6: Expand statistics**

  Add `out_of_range_packets`, `fully_missing_groups`, `expected_station_packets`, `large_gap_advances`, `payload_copy_calls`, and `payload_copy_bytes`. Remove raw-block-age eviction semantics from decisions; retain any raw block sequence only as diagnostic input.

- [x] **Step 7: Run the engine and timeline tests three times**

  ```bash
  cmake --build build-atfp-local --target vdif_timeline_test vdif_unpack_engine_test
  ctest --test-dir build-atfp-local -R '^(vdif_timeline_test|vdif_unpack_engine_test)$' --repeat until-pass:3 --output-on-failure
  ```

- [x] **Step 8: Perform the Task 3 server handoff and wait for callback**

  Request Release tests plus a CPU microbenchmark comparing the legacy 4096-byte/2-byte-scatter path with ATFP. Acceptance requires exactly one payload placement copy per accepted packet, byte equivalence, wrap/zero-fill correctness, and O(1) slot lookup.

---

### Task 4: Single-Block ATFP Writer and PSRDADA Worker Integration

**Status:** Completed; qths1 Release integration passed three consecutive clean
rounds for full, partial, fully missing and continuous double-transfer cases.

**Files:**
- Create: `include/rdma_dada/modules/vdif_unpack/atfp_block_writer.h`
- Create: `modules/vdif_unpack/atfp_block_writer.cpp`
- Create: `tests/atfp_block_writer_test.cpp`
- Modify: `apps/vdif_unpack_worker/main.cpp`
- Modify: `apps/vdif_unpack_worker/README.md`
- Modify: `tests/vdif_unpack_worker_integration.sh`
- Modify: `CMakeLists.txt`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: synchronous `AtfpBlockView`, `pipeline::WritableBlockSink`, compute-block capacity.
- Produces:

```cpp
class AtfpBlockWriter {
public:
    bool Configure(std::uint64_t block_capacity,
                   pipeline::WritableBlockSink* sink,
                   std::string* error);
    bool Write(const AtfpBlockView& view, std::string* error);
    bool Finish(std::string* error);
};
```

- [x] **Step 1: Add a failing sink-lifecycle test**

  The fake sink rejects a second `Acquire` while a block is open and records acquire/commit counts. Test no-wrap, wrap, full block, compact partial block, invalid view geometry, insufficient capacity, acquire failure, commit failure, and `Finish` with no open block.

- [x] **Step 2: Confirm the writer test fails before implementation**

- [x] **Step 3: Implement one-acquire/one-commit publication**

  For actual `G`, require `bytes=NANT*G*payload_bytes <= capacity`. Acquire once. For each antenna, copy one source segment or two disjoint segments at circular wrap into `destination + a*G*payload_bytes`. Commit exactly `bytes` once. Never acquire while another block is open and never wait for network input after acquire.

- [x] **Step 4: Replace `GroupBlockWriter` only in `vdif_unpack_worker`**

  Keep generic `pipeline::GroupBlockWriter` unchanged. Parse the timeline in `OpenTransfer`, configure engine and writer per transfer, pass `AtfpBlockView` synchronously from `ConsumeRawBlock/Finish`, and update log statistics with ring-acquire wait, publication bytes/calls, fully missing groups and expected loss denominator.

- [x] **Step 5: Update integration fixtures to ATFP**

  Verify full, partial, circular-wrap, fully missing group, header/EOD and continuous double-transfer cases. The expected file is block-scoped: `Block0[A,T0,F,P] | Block1[A,T1,F,P]`.

- [x] **Step 6: Run portable writer/engine tests three times**

- [x] **Step 7: Perform the Task 4 server handoff and wait for callback**

  Phase 0 must locate PSRDADA binaries and library paths before building. Run Release `atfp_block_writer_test`, `vdif_unpack_engine_test`, and `vdif_unpack_worker_integration_test` three clean repetitions. Acceptance requires at most one writable block held and exact acquire=commit counts.

---

### Task 5: Publish the Finite-Transfer Raw EOD Tail

**Status:** Completed. The HF-controlled versioned runner accepted three clean
0.1 Gbps finite transfers in suite `20260809T011953Z-38a07dd9`, with exact
sender/receiver/raw/unpack/compute accounting, CQ-tail publication, partial raw
block handling, ATFP bytes, EOD, and scoped cleanup.

**Files:**
- Modify: `include/rdma_dada/io/psrdada/ring_writer.h`
- Modify: `src/io/psrdada/ring_writer.cpp`
- Modify: `src/io/rdma/receiver.cpp`
- Modify: `apps/rdma2dada/main.cpp`
- Modify: `tests/rdma_receive_policy_test.cpp`
- Modify: `tests/rdma_receiver_integration.sh`
- Modify: `apps/rdma2dada/README.md`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: completed accepted receive records accumulated in the currently open raw block when stop/EOD occurs.
- Produces: one optional partial raw block containing only complete `32-byte header + payload` records, followed by EOD; accepted/published/tail counters.

- [x] **Step 1: Add failing pure accounting tests**

  Verify zero tail, one record, `work_num` records, a tail one record below full block, and rejection of byte counts not divisible by `raw_record_bytes`.

- [x] **Step 2: Implement explicit partial-block publication**

  Track current open-block valid bytes. On orderly stop, finish polling/reposting ownership for completed WRs, publish the valid complete-record prefix with `MarkWritten(valid_bytes)`, then send EOD. Do not publish uncompleted receive buffers and do not count the WR depth as network loss.

- [x] **Step 3: Add low-rate Linux integration coverage**

  Send a record count deliberately not divisible by `records_per_raw_block`; verify receiver accepted equals raw published equals unpack consumed and the final compute data is complete.

- [x] **Step 4: Perform the Task 5 server handoff and wait for callback**

  Run three clean low-rate finite transfers with dbdisk inspection. The
  authoritative multi-host command runs `scripts/task8c_rate_point.py` on HF,
  which opens independent sessions to qths1 and both senders; do not run the
  receiver-local shell fixture on qths1. Acceptance requires exact record
  conservation and no ring/process/capability residue.

---

### Task 6: CPU ATFP-to-TFPA Complex Conversion Contract

**Files:**
- Modify: `modules/complex_convert/complex_convert_module.cpp`
- Modify: `include/rdma_dada/modules/complex_convert/complex_convert_module.h`
- Modify: `tests/complex_convert_module_test.cpp`
- Modify: `modules/complex_convert/README.md`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: one block-scoped `UNPACKED/ATFP/CI8|CI16`, IQ, little-endian, two's-complement block and `CONVERSION_SCALE`.
- Produces: physically contiguous `CONVERTED/TFPA/CF32`, component order RI, with `SOURCE_ORDER=ATFP` and no signedness metadata fields.

- [ ] **Step 1: Add failing asymmetric CPU oracle tests**

  Cover CI8 and CI16, `A=3`, non-square `Q=T*F*P`, negative extrema, non-unit scale, partial T, invalid byte remainder, insufficient output, overlapping buffers, invalid order/encoding/component order, and scale applied exactly once.

- [ ] **Step 2: Implement block-scoped geometry**

  Derive `T=input.size/(A*F*P*input_sample_bytes)` per block. For every `a,q`, convert source index `a*Q+q` to destination index `q*A+a`. Preserve input sequence and set exact output bytes.

- [ ] **Step 3: Remove signedness fields**

  Require `SAMPLE_ENCODING=TWOS_COMPLEMENT`; infer signed CI8/CI16 from sample format; erase both old signedness fields in output metadata.

- [ ] **Step 4: Run CPU conversion tests three times and hand off**

  Server acceptance uses `USE_CUDA=OFF` Release and exact CF32 comparison with the independent oracle.

---

### Task 7: Fused CUDA Physical Transpose, Conversion and Scale

**Files:**
- Modify: `modules/complex_convert/complex_convert_backend.h`
- Modify: `modules/complex_convert/complex_convert_cuda_backend.cu`
- Modify: `modules/complex_convert/complex_convert_module.cpp`
- Modify: `tests/complex_convert_cuda_test.cpp`
- Create: `tools/complex_convert_transpose_cuda_benchmark.cpp`
- Modify: `scripts/build_gpu_tests.sh`
- Modify: `CMakeLists.txt`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: device-resident `[A,Q]` CI8 or CI16 and caller-owned non-default CUDA stream.
- Produces: device-resident `[Q,A]` CF32 without internal synchronization.

- [ ] **Step 1: Add failing GPU tests**

  Cover CI8/CI16, negative extrema, scale, `A=3`, `Q<tile`, non-tile multiples, large non-square matrices, partial T, wrong device, null/default stream, and exact comparison with Task 6 CPU output.

- [ ] **Step 2: Change the backend API to carry A and Q**

  `CudaComplexConvertExecutor::Process` receives `nant` and `q` instead of only a flat sample count. Reject products that overflow launch/index ranges.

- [ ] **Step 3: Implement the fused tiled kernel**

  Use padded shared memory for `[A,Q] -> [Q,A]`; load signed IQ, convert to float, multiply by scale once, and store `Complex32`. Launch on `context.native_stream`; do not call `cudaDeviceSynchronize` or `cudaStreamSynchronize` in `Process`.

- [ ] **Step 4: Add diagnostic benchmark boundaries**

  Report H2D separately from kernel time, effective input/output GB/s, A/Q geometry and warm-up. The benchmark is diagnostic and does not replace correctness tests.

- [ ] **Step 5: Hand off Task 7 and wait for GPU callback**

  Require RTX 3090, CUDA 12.8, Release build, three correctness repetitions and benchmark warm-up plus three measured runs. Record GPU/binary/config SHA256 and preserve complete logs.

---

### Task 8: Integrate Conversion as the First GPU Algorithm Stage

**Files:**
- Modify: `apps/pipeline_worker/main.cpp`
- Modify: `apps/pipeline_worker/README.md`
- Modify: `src/pipeline/worker_config.cpp`
- Modify: `include/rdma_dada/pipeline/worker_config.h`
- Modify: `config/pipeline_worker.example.json`
- Modify: `tests/pipeline_worker_core_test.cpp`
- Modify: `tests/pipeline_worker_cuda_chain_test.cpp`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: host compute ring `UNPACKED/ATFP/CI8`, conversion configuration, beamform weights, and optional Power/Stokes/TimeIntegrate stages.
- Produces: `H2D -> ATFP transpose/convert -> beamform -> optional product -> optional integrate -> D2H` on one worker-owned stream.

- [ ] **Step 1: Add failing configuration and chain tests**

  Verify conversion is mandatory before beamform, Power and Stokes remain mutually selected as configured, integration remains last, and scratch capacities use actual input block T. Reject a TFPA integer ring at the new worker boundary.

- [ ] **Step 2: Insert `ComplexConvertModule` after H2D**

  Allocate distinct device regions for integer ATFP input, converted TFPA CF32, beamformed TFPB and optional unintegrated output. Do not alias conversion input/output. Keep one CUDA stream and the existing per-block final synchronization.

- [ ] **Step 3: Propagate metadata through every stage**

  Require `UNPACKED/ATFP -> CONVERTED/TFPA -> BEAMFORMED/TFPB`; preserve actual T and scale byte-rate/transfer fields correctly.

- [ ] **Step 4: Run portable config tests and hand off full GPU chain**

  GPU acceptance uses known asymmetric packet values and FPAB weights, then verifies D2H numerical output for beamform-only, Power, Stokes (`NPOL=2`) and integration combinations three times.

---

### Task 9: Reproducible PSRDADA/UDP Acceptance and Remaining Rate Tests

**Files:**
- Modify only when required by demonstrated harness gaps: `scripts/task8c_rate_point.py`
- Modify with every controller behavior change: `tests/task8c_rate_point_test.py`
- Modify: `docs/agents/testing.md`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: versioned controller, Release binaries, two equal-rate UDP Station senders, qths1 receiver/raw ring/unpack/compute consumer, optional GPU pipeline.
- Produces: machine-readable correctness, performance, cleanup and notification evidence.

- [ ] **Step 1: Run Phase 0 before changing the harness**

  Record exact qths1 PSRDADA and project binary paths/options/SHA, qtp sender paths, Python/CMake/compiler/CUDA versions, dynamic libraries, permissions, NIC/link/NUMA, CPU/memory/disk, clock state and non-login shell behavior. Do not run Git on servers.

- [ ] **Step 2: Update the versioned runner for ATFP evidence**

  Parse the new counters/header, select `dada_dbnull -s -z` for throughput, retain dbdisk only for byte inspection, abort all processes when either Station fails, capture per-run NIC counter deltas and CPU/NUMA placement, and keep cleanup from overwriting test outcome.

- [ ] **Step 3: Complete the unfinished low-rate acceptance**

  Run one warm-up plus three measured `0.1 Gbps` repetitions from a clean state. Require exact sender/receiver/raw/unpack/compute counts, ATFP bytes, no unintended missing groups, clean EOD and cleanup PASS in every repetition.

- [ ] **Step 4: Repeat the 1 Gbps dbnull gate**

  Run one warm-up plus three measured repetitions. Compare Release legacy TFPA and Release ATFP parsing, placement, output-ring wait and total throughput. Do not proceed if any correctness gate fails.

- [ ] **Step 5: Continue the coarse rate scan only while lower points pass**

  Test aggregate payload rates `5, 10, 20, 30, 35, 38 Gbps`, each with one warm-up plus three measured runs. Report payload and estimated wire rates separately; identify the last stable and first failing point without selecting only the best run.

- [ ] **Step 6: Run deterministic low-loss tests below the stable rate**

  Inject `0.001%`, `0.01%`, and `0.1%` packet errors/loss. Verify missing Station and fully missing group counters, exact zero-filled ATFP positions, raw partial EOD conservation, continuous observation semantics and numerical GPU output.

- [ ] **Step 7: Return final callback and stop for the user's Git decision**

  Require three clean functional repetitions and full result artifacts. After callback PASS, development reports the tested scope and reminds the user that the changes are ready for commit; it does not commit or push.
