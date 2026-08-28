# GPU Multi-Stream Block Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded multi-stream CUDA block pipeline that overlaps H2D, algorithms, and D2H while preserving PSRDADA ring leases and strict output-block order.

**Architecture:** `pipeline_worker` retains the current `SYNCHRONOUS_DIRECT` executor and gains a `STAGED_PIPELINE` executor behind one block-execution seam. The implemented staged executor registers the compute ring with CUDA, retains each input lease until H2D completes, enqueues one block per slot stream, and uses one ordered writer to publish pinned output buffers to the output ring. This supersedes the original pinned-input staging step described in early tasks below.

**Tech Stack:** C++11, CUDA Runtime 12.x, cuBLAS, PSRDADA, JSON configuration, CMake/CTest, Python controller tests.

**Spec:** `docs/superpowers/specs/2026-08-27-gpu-multistream-pipeline-design.md`

## Global Constraints

- Do not modify PSRDADA, packet formats, unpack, algorithm kernels, ring block formats, or weight files.
- Preserve `SYNCHRONOUS_DIRECT` byte-for-byte as the matched performance baseline.
- `STAGED_PIPELINE` permits 1–4 in-flight blocks and always publishes by logical input sequence.
- CUDA must never reference a PSRDADA input block after its lease is released.
- Only one writer may call output-ring `ipcio_open_block_write` and `ipcio_close_block_write`.
- All queues and buffers are bounded by `inflight_blocks`; no drop-on-pressure behavior is allowed.
- EOD succeeds only after all submitted slots are published in order.
- A failed sequence prevents that block and every later sequence from being published.
- Do not create a Git commit automatically. Repository rules require GPU-server acceptance first and the user decides when to commit.
- Keep the design and plan files out of the final product commit unless the user explicitly requests otherwise.

---

## File structure

### New focused files

- `include/rdma_dada/pipeline/gpu_block_pipeline.h`: public execution mode, configuration, publish function, lifecycle, and metrics interface.
- `src/pipeline/gpu_block_pipeline.cpp`: synchronous and staged implementations, input-ring leases, device/output-pinned slot ownership, CUDA events, ordered writer, drain, and abort.
- `include/rdma_dada/pipeline/ordered_slot_scheduler.h`: portable bounded slot-state and sequence-order interface.
- `src/pipeline/ordered_slot_scheduler.cpp`: portable scheduler implementation with no CUDA or PSRDADA dependency.
- `tests/ordered_slot_scheduler_test.cpp`: deterministic order, pressure, EOD, and failure tests.
- `tests/gpu_block_pipeline_cuda_test.cpp`: CUDA numerical equivalence, completion reordering, and slot isolation tests.

### Existing files changed by responsibility

- Observation/config compiler: schema, parsed config, resolved plan, JSON serializer, worker config.
- GPU budget: per-slot device and pinned-host accounting.
- `apps/pipeline_worker/main.cpp`: PSRDADA adapter and executor selection only.
- Worker metrics: mode, slot, ring registration, zero input staging, ordering,
  output staging, and wait measurements.
- Controller/profile/catalog: record and compare execution mode and slot count.
- CMake/tests/docs/source manifest: build, acceptance, and reproducibility.

---

### Task 1: Observation and resolved-plan execution contract

**Files:**
- Modify: `config/observation-v1.schema.json`
- Modify: `include/rdma_dada/config/observation_config.h`
- Modify: `src/config/observation_config.cpp`
- Modify: `include/rdma_dada/config/resolved_observation_plan.h`
- Modify: `src/config/resolved_observation_plan.cpp`
- Modify: `src/config/resolved_plan_json.cpp`
- Modify: `include/rdma_dada/pipeline/worker_config.h`
- Modify: `src/pipeline/worker_config.cpp`
- Test: `tests/observation_config_test.cpp`
- Test: `tests/resolved_observation_plan_test.cpp`
- Test: `tests/worker_resolved_plan_test.cpp`

**Interfaces:**
- Produces: `enum class CudaPipelineMode { kSynchronousDirect, kStagedPipeline };`
- Produces: parsed and resolved fields `cuda_pipeline_mode` and `cuda_inflight_blocks`.
- Consumes: existing Observation JSON and resolved-plan identity machinery.

- [ ] **Step 1: Write failing schema/parser tests**

Add cases equivalent to:

```cpp
EXPECT_PARSE("SYNCHRONOUS_DIRECT", 1);
EXPECT_PARSE("STAGED_PIPELINE", 1);
EXPECT_PARSE("STAGED_PIPELINE", 3);
EXPECT_REJECT("STAGED_PIPELINE", 0);
EXPECT_REJECT("STAGED_PIPELINE", 5);
EXPECT_REJECT("SYNCHRONOUS_DIRECT", 2);
EXPECT_DEFAULT("SYNCHRONOUS_DIRECT", 1);
```

- [ ] **Step 2: Run the focused tests and confirm RED**

Run the existing portable CMake test target or direct test executable for
`observation_config_test`, `resolved_observation_plan_test`, and
`worker_resolved_plan_test`. Expected failure: unknown `cuda_pipeline` field or
missing `cuda_pipeline_mode` member.

- [ ] **Step 3: Add the parsed and resolved types**

Use the exact enum and fields:

```cpp
enum class CudaPipelineMode {
    kSynchronousDirect,
    kStagedPipeline
};

CudaPipelineMode cuda_pipeline_mode;
std::uint32_t cuda_inflight_blocks;
```

The Observation parser defaults an omitted object to direct/1. The resolved
JSON always emits:

```json
"cuda_pipeline":{"mode":"SYNCHRONOUS_DIRECT","inflight_blocks":1}
```

- [ ] **Step 4: Centralize validation in the compiler path**

Reject direct mode with slots other than one and staged mode outside 1–4 before
ring creation. Include both fields in configuration identity and copy them into
`WorkerConfig`; do not let `pipeline_worker` reinterpret raw JSON.

- [ ] **Step 5: Run focused and identity tests and confirm GREEN**

Run the three focused tests plus `tests/config_identity_test.cpp`. Expected:
all pass, and changing only mode or slot count changes `CONFIG_ID`.

- [ ] **Step 6: Record checkpoint**

Capture `git diff --check`, affected-file list, and test counts without committing.

---

### Task 2: Slot-aware GPU and pinned-host budget

**Files:**
- Modify: `include/rdma_dada/config/gpu_pipeline_budget.h`
- Modify: `src/config/gpu_pipeline_budget.cpp`
- Modify: `src/config/observation_artifacts.cpp`
- Modify: `tools/observation_config_compile.cpp`
- Test: `tests/observation_artifacts_test.cpp`
- Test: `tests/observation_config_compile_test.py`

**Interfaces:**
- Consumes: resolved `cuda_pipeline_mode`, `cuda_inflight_blocks`, existing block byte counts, and weight bytes.
- Produces: `device_bytes_per_slot`, `slot_device_bytes_total`, `pinned_input_bytes=0`, `pinned_output_bytes`, and `planned_pinned_host_bytes`.

- [ ] **Step 1: Write failing exact-arithmetic tests**

For staged mode with three slots assert:

```text
device_bytes_per_slot = input + converted + scratch + output
slot_device_bytes_total = device_bytes_per_slot * 3
planned_device_bytes = slot_device_bytes_total + weights
pinned_input_bytes = 0
pinned_output_bytes = output_block_bytes * 3
planned_pinned_host_bytes = pinned_input_bytes + pinned_output_bytes
```

For direct mode assert pinned bytes are zero and existing single-buffer device
budget remains unchanged. Staged mode budgets only pinned output because the
compute ring is registered directly.

- [ ] **Step 2: Run budget tests and confirm RED**

Expected failure: new fields are absent from the budget JSON.

- [ ] **Step 3: Implement checked multiplication and reporting**

Use existing checked arithmetic helpers. Reject overflow and report mode/slots in
`preflight.json`; retain the statement that CUDA runtime and library workspace
are excluded.

- [ ] **Step 4: Run compiler tests and confirm GREEN**

Run `observation_artifacts_test` and `observation_config_compile_test.py` with
direct, staged-1, staged-3, invalid-0, and invalid-5 fixtures.

- [ ] **Step 5: Record checkpoint without committing**

Save exact expected budget values for the current Power+Integration geometry.

---

### Task 3: Portable ordered slot scheduler

**Files:**
- Create: `include/rdma_dada/pipeline/ordered_slot_scheduler.h`
- Create: `src/pipeline/ordered_slot_scheduler.cpp`
- Create: `tests/ordered_slot_scheduler_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:

```cpp
enum class SlotState { kFree, kSubmitted, kCompleted, kPublishing, kFailed };

struct SlotLease {
    std::uint32_t slot_index;
    std::uint64_t sequence;
};

class OrderedSlotScheduler {
public:
    explicit OrderedSlotScheduler(std::uint32_t slots);
    StageStatus Acquire(std::uint64_t sequence, SlotLease* lease);
    StageStatus MarkCompleted(const SlotLease& lease);
    StageStatus NextPublishable(SlotLease* lease) const;
    StageStatus MarkPublished(const SlotLease& lease);
    StageStatus MarkFailed(const SlotLease& lease,
                           const std::string& message);
    bool empty() const;
    bool failed() const;
};
```

- [ ] **Step 1: Write scheduler tests first**

Cover:

```text
Acquire sequences 0,1,2 into three distinct slots
Complete 1 before 0: NextPublishable remains unavailable
Complete 0: publish 0, then publish 1
Fourth acquire fails/blocks while all three slots are occupied
Publishing releases exactly the matching slot
Failure at sequence 1 prevents publishing sequences >=1
Duplicate/stale lease operations are rejected
```

- [ ] **Step 2: Build and confirm RED**

Expected failure: scheduler header or symbols do not exist.

- [ ] **Step 3: Implement the minimal state machine**

Keep sequence-to-slot ownership internal and bounded by the constructor slot
count. Do not add CUDA, PSRDADA, threads, or timing to this module.

- [ ] **Step 4: Run scheduler tests and confirm GREEN**

Run the executable repeatedly; expected deterministic pass with no allocations
after construction.

- [ ] **Step 5: Record checkpoint without committing**

Review the interface using the deletion test: CUDA and ring code must not leak
into scheduler callers.

---

### Task 4: GPU block-pipeline seam and synchronous baseline adapter

**Files:**
- Create: `include/rdma_dada/pipeline/gpu_block_pipeline.h`
- Create: `src/pipeline/gpu_block_pipeline.cpp`
- Modify: `apps/pipeline_worker/main.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/pipeline_worker_core_test.cpp`
- Test: `tests/pipeline_worker_cuda_chain_test.cpp`
- Test: `tests/pipeline_worker_cuda_products_test.cpp`

**Interfaces:**
- Consumes: `WorkerConfig`, `WorkerGeometry`, input metadata, and a publish function.
- Produces:

```cpp
struct OutputBlockFunctions {
    AcquireOutputBlockFunction acquire;
    CommitOutputBlockFunction commit;
    AbortOutputBlockFunction abort;
};

class GpuBlockPipeline {
public:
    GpuBlockPipeline();
    ~GpuBlockPipeline();
    StageStatus Configure(const WorkerConfig& config,
                          const WorkerGeometry& geometry,
                          const Metadata& input_header,
                          const OutputBlockFunctions& output,
                          Metadata* output_header);
    StageStatus SubmitBlock(std::uint64_t sequence,
                            const std::uint8_t* ring_data,
                            std::uint64_t input_bytes);
    StageStatus Drain();
    StageStatus Abort(const std::string& reason);
    StageStatus Finish();
    const WorkerMetrics& metrics() const;
};
```

- [ ] **Step 1: Add a regression test around the current direct path**

Use the existing CUDA fixtures to capture exact block counts, input/output byte
counts, output data, header, and EOD for `SYNCHRONOUS_DIRECT`.

- [ ] **Step 2: Run the regression and confirm the new interface is RED**

Expected failure: `GpuBlockPipeline` does not exist.

- [ ] **Step 3: Move, do not rewrite, current CUDA execution into the direct adapter**

The direct adapter must retain one non-blocking stream, direct ring-pointer H2D,
direct output-ring D2H, one stream synchronization per block, and existing event
metrics. It acquires the output block before D2H and commits it inline. The
staged writer uses the same seam but copies pinned output into the acquired ring
block before committing, so direct mode gains no extra host copy.

- [ ] **Step 4: Make `pipeline_worker` select the adapter from WorkerConfig**

Keep CPU_REFERENCE unchanged. Replace the CUDA branch in `ProcessBlock` with
`SubmitBlock`; call `Drain` before output EOD and `Finish` during cleanup.

- [ ] **Step 5: Run all existing CUDA worker tests and confirm GREEN**

Require exact numerical and lifecycle equality with commit `058f4bc`; record any
intentional log-only difference explicitly.

- [ ] **Step 6: Record checkpoint without committing**

Do not start staged implementation until the preserved direct adapter is green.

---

### Task 5: Registered compute-ring slots and asynchronous CUDA enqueue

**Files:**
- Modify: `src/pipeline/gpu_block_pipeline.cpp`
- Modify: `include/rdma_dada/pipeline/gpu_block_pipeline.h`
- Create: `tests/gpu_block_pipeline_cuda_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 3 scheduler and Task 4 pipeline interface.
- Produces: staged slot allocation, asynchronous submission, and completion events.

- [ ] **Step 1: Write CUDA tests for slot isolation and numerical equivalence**

Test staged slots 1, 2, 3, and 4 against the direct output for Beamform-only,
Beamform+Power, Beamform+Stokes, and Power/Stokes+Integration fixtures.

- [ ] **Step 2: Run tests and confirm RED**

Expected failure: staged mode reports unsupported or does not allocate slots.

- [ ] **Step 3: Register the compute ring and allocate pinned output buffers**

Register all compute-ring data blocks once per transfer with
`dada_cuda_dbregister`; unregister only after all slot work is synchronized.
For each slot allocate pinned output storage, one stream, events and all device
buffer classes. Roll back already-created resources on partial allocation
failure.

- [ ] **Step 4: Implement sequential submission with asynchronous CUDA work**

`SubmitBlock` waits for a free slot, retains the ring lease, enqueues H2D
directly from the registered ring block, then conversion, module chain, D2H and
completion events. Release the ring lease after H2D completes; later work uses
device buffers only.

- [ ] **Step 5: Run numerical tests and confirm GREEN**

Require exact integer-derived results and existing floating tolerances. Verify no
slot buffer address is shared and no stream is reused before slot release.

- [ ] **Step 6: Record checkpoint without committing**

Capture planned versus allocated device and pinned bytes.

---

### Task 6: Single ordered output writer, drain, and failure propagation

**Files:**
- Modify: `src/pipeline/gpu_block_pipeline.cpp`
- Modify: `apps/pipeline_worker/main.cpp`
- Test: `tests/ordered_slot_scheduler_test.cpp`
- Test: `tests/gpu_block_pipeline_cuda_test.cpp`
- Test: `tests/pipeline_worker_resolved_integration.sh`

**Interfaces:**
- Consumes: staged slot completion events and `OutputBlockFunctions`.
- Produces: writer thread with strict sequence publication and bounded backpressure.

- [ ] **Step 1: Write failing order and EOD tests**

Use a deterministic CUDA delay fixture so sequence 1 completes before sequence
0; assert publication is still 0 then 1. Add partial-final-block, full-output-ring,
EOD-with-three-active-slots, and injected-failure-at-sequence-1 cases.

- [ ] **Step 2: Run and confirm RED**

Expected failure: completion-order output or missing asynchronous writer/drain.

- [ ] **Step 3: Implement one writer thread**

The writer waits only for `next_publish_sequence`, synchronizes that slot's
completion event, checks asynchronous CUDA status, invokes the publish function,
marks the slot published, and signals blocked submitters. No other thread may
touch output `ipcio` data blocks.

- [ ] **Step 4: Implement drain and abort**

`Drain` stops new submission, waits until submitted equals published, joins the
writer, and returns the first error. `Abort` stores the first failure, rejects new
blocks, prevents publication from the failed sequence onward, wakes all waiters,
joins the writer, and releases resources without reporting successful EOD.

- [ ] **Step 5: Run order, failure, partial, EOD, and integration tests**

Expected: ordered output, exact actual byte sizes, no deadlock, non-zero result on
injected failure, and no ring/process residue.

- [ ] **Step 6: Record checkpoint without committing**

Inspect thread ownership: reader owns input callback, writer alone owns output
data blocks, and transfer close calls drain before module finish.

---

### Task 7: Metrics, controller, profile, and catalog integration

**Files:**
- Modify: `include/rdma_dada/pipeline/worker_metrics.h`
- Modify: `src/pipeline/worker_metrics.cpp`
- Modify: `scripts/task8c_rate_point.py`
- Modify: `scripts/task8c_profiles.py`
- Modify: `scripts/task8c_catalog.py`
- Modify: `config/testing/profile.schema.json`
- Modify: `config/testing/test-result-catalog.schema.json`
- Test: `tests/pipeline_worker_metrics_test.cpp`
- Test: `tests/task8c_rate_point_test.py`
- Test: `tests/task8c_profiles_test.py`
- Test: `tests/task8c_catalog_test.py`

**Interfaces:**
- Consumes: resolved execution mode, slot count, and pipeline metrics.
- Produces: reproducible result fields for A/B comparisons.

- [ ] **Step 1: Write failing metrics and artifact tests**

Require these exact fields:

```text
execution_mode, inflight_blocks, submitted_blocks, completed_blocks,
published_blocks, max_inflight, completion_reorder_count,
slot_wait_ns_total/max, writer_wait_ns_total/max,
input_staging_copy_ns_total/max, output_staging_copy_ns_total/max,
input_staging_bytes, output_staging_bytes,
planned_device_bytes, planned_pinned_host_bytes
```

- [ ] **Step 2: Run tests and confirm RED**

Expected failure: absent metrics and catalog/profile fields.

- [ ] **Step 3: Add low-overhead metric recording**

Use CUDA events for device stages and host clocks only at acquire, host-copy,
completion, publish, and wait transitions. Do not add clocks to polling loops.

- [ ] **Step 4: Make controller validation mode-aware**

Require submitted=completed=published for successful staged runs, exact input
and output byte accounting, output dbnull EOD, and cleanup. Record the resolved
mode and slots; do not add a duplicate CLI override for scientific configuration.

- [ ] **Step 5: Run all metrics/controller/profile/catalog tests**

Expected: direct legacy profiles remain valid, staged profiles require explicit
mode/slots, and comparable-result queries expose both fields.

- [ ] **Step 6: Record checkpoint without committing**

Generate a dry-run compact result and verify no duplicate logs or configuration
artifacts are retained.

---

### Task 8: Documentation, source identity, and complete local verification

**Files:**
- Modify: `apps/pipeline_worker/README.md`
- Modify: `doc/ALGORITHM_MODULE_CONTRACTS.md`
- Modify: `doc/DEVELOPMENT_PLAN.md`
- Modify: `docs/PROJECT_STATUS.md`
- Modify: `docs/agents/testing.md`
- Modify: `tests/README.md`
- Modify: `config/testing/atfp-throughput-source-manifest.sha256`

**Interfaces:**
- Consumes: all implemented configuration, metrics, and test entry points.
- Produces: reproducible developer and GPU-server acceptance instructions.

- [ ] **Step 1: Document both modes and ring compatibility**

State that staged mode registers the compute ring for direct H2D, retains each
input lease until H2D completion, uses private pinned output staging, permits
completion reordering but not publication reordering, and propagates bounded
backpressure through the existing rings.

- [ ] **Step 2: Document exact server comparison matrix**

Record:

```text
A: all NUMA1 + SYNCHRONOUS_DIRECT
B: ingress1/processing0 + SYNCHRONOUS_DIRECT
C1..C4: ingress1/processing0 + STAGED_PIPELINE slots1..4
```

All other observation, ring, CPU, queue, binary, and sender parameters must match.

- [ ] **Step 3: Run the full locally available suite**

Run portable config/header tests, scheduler tests, worker core tests, Python
controller/profile/catalog/artifact tests, `git diff --check`, and deterministic
source-manifest regeneration/verification. Record skipped CUDA/PSRDADA tests as
server-required rather than passing.

- [ ] **Step 4: Refresh documentation status accurately**

Mark development complete/server pending only after local gates pass. Do not
claim sustained-rate acceptance from isolated kernels or one finite run.

- [ ] **Step 5: Notify `GPU服务器代码测试`**

Handoff affected files, manifest SHA, validated build directory requirements,
test commands, fixtures, expected output ordering, and cleanup criteria. Request
Phase 0, fresh CUDA Release build, direct/CUDA/integration tests x3, 1 Gbps staged
functional test, then the matched A/B/C 30 Gbps diagnostics. The testing task
owns remote synchronization and execution.

- [ ] **Step 6: Wait for explicit server callback**

Keep TEST_RESULT, CLEANUP_RESULT, and notification delivery independent. If
functional tests pass, tell the user the changes are ready for their commit
decision; do not commit or push automatically.
