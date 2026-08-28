# 30 Gbps Single-GPU and Full-Pipeline Acceptance Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish repeatable 30 Gbps acceptance first for the single-GPU compute/output path and then for the complete UDP receive → unpack → GPU → output path.

**Architecture:** The GPU-only topology uses a repository-owned pressure writer that publishes the compiler-generated header byte-for-byte and then paces complete data blocks against a monotonic clock. It feeds a CUDA-registered compute ring, one RTX 3090 running the three-slot staged pipeline, an output ring, and `dada_dbnull`. The full topology inherits the accepted qths1 30 Gbps unpack profile unchanged and adds the already validated CUDA-registered staged GPU worker; it does not rerun standalone receiver or unpack performance suites.

**Tech Stack:** Python 3 controller, CMake/CTest 3.31.12, CUDA 12.8/SM86, PSRDADA, repository GPU pressure writer, `dada_dbnull`, UDP sender simulator.

**Spec:** `docs/superpowers/specs/2026-08-27-gpu-multistream-pipeline-design.md`

**Current status (2026-08-28):** The Full topology has passed 30 Gbps for
60 seconds with one warm-up and three measured repetitions using the accepted
ingest/unpack profile and staged/3 CUDA pipeline. GPU-only pressure work in
Tasks 1, 3 and 4 is paused; `gpu_pressure_writer` is not implemented and stock
`dada_junkdb` is not an accepted substitute. The unchecked steps below are
retained as future work, not as claims of completed implementation.

## Global Constraints

- Canonical source directory on HF/qths1/qtpulsar1/qtpulsar2 is `/home/user/wy/rdma_dada`; synchronize it incrementally with `rsync`, never with versioned source archives.
- Verify the complete current source manifest on all four hosts before configuring.
- Use `/home/user/wy/tools/cmake-3.31.12/bin/cmake` and the paired `ctest` on qths1.
- Preserve the accepted unpack profile `config/testing/profiles/qths1-unpack-30gbps-60s-v1.json` without silent changes.
- Preserve Station source ports 45871/55871, one-second preparation, receiver CPU 13, unpack CPUs 14–19, sink CPU 20, GPU CPU 21, NUMA node 1, poll batch 32, WR count 1024, raw ring 16 blocks, compute ring 8 blocks, and output ring 8 blocks.
- Production geometry is compute block 52,428,800 B and output block 819,200 B. At 30 Gbps, GPU-only pacing is `ceil(3,750,000,000 / 52,428,800) = 72` blocks/s, 3,774,873,600 B/s = 30.1989888 Gbps, and 2,160 blocks in 30 s.
- A single diagnostic is not final performance acceptance. Formal acceptance is one warm-up followed by three consecutive measured runs.
- Stop at the first failed gate, preserve the compact failure evidence, and clean only recorded task-owned resources.
- Do not run a separate `rdma2dada` or `rdma2dada+unpack` performance suite; their accepted 30 Gbps evidence is reused.
- Do not claim empirical 20% headroom from a 30 Gbps run. The 20% deadline reserve remains a design/budget requirement until a separate 37.5 Gbps headroom experiment passes.

---

### Task 1: Add an exact-header, block-paced GPU pressure writer

**Files:**
- Modify: `CMakeLists.txt`
- Create: `include/rdma_dada/testing/gpu_pressure_plan.h`
- Create: `src/testing/gpu_pressure_plan.cpp`
- Create: `apps/gpu_pressure_writer/main.cpp`
- Modify: `scripts/task8c_rate_point.py`
- Create: `tests/gpu_pressure_plan_test.cpp`
- Test: `tests/task8c_rate_point_test.py`
- Modify: `docs/agents/testing.md`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: compiler-generated `unpacked.header` and the requested GPU pressure rate/duration.
- Produces: `gpu_pressure_writer` CLI with `--key`, `--header`, `--block-bytes`, `--blocks-per-second`, `--block-count` and `--fill-byte` arguments; for the 30-second production run their values are `00d4`, the run-owned compiler header path, `52428800`, `72`, `2160` and `0`. It publishes the supplied 4,096-byte header without changing any field, then writes exactly 2,160 complete blocks at monotonic absolute deadlines.

- [ ] **Step 1: Add failing strict-header and pacing tests**

  For a 30 Gbps GPU-only 30-second plan, assert 72 blocks/s, 2,160 blocks and 113,246,208,000 total bytes. Assert the writer consumes the compiler's `unpacked.header` byte-for-byte; `BYTES_PER_SECOND` and `FILE_SIZE` retain compiler values, and compiler-generated `TRANSFER_SIZE` equals 113,246,208,000. Assert block deadline `n` is calculated from one fixed monotonic epoch rather than the previous completion time.

- [ ] **Step 2: Run the tests and verify RED**

  Run `python3 -B tests/task8c_rate_point_test.py -q` and build/run `gpu_pressure_plan_test`.

  Expected: failure because the controller currently rewrites header fields and stock `dada_junkdb` both publishes/mutates the header and uses one-second pacing quanta.

- [ ] **Step 3: Align the GPU-only compiled transfer to complete blocks**

  In `compile_rate_plan`, after bootstrap geometry is known, make the GPU topology choose `group_count = blocks_per_second * duration_seconds * groups_per_block`. Set the generated observation duration from that group count before the final compiler invocation. The compiler must emit the exact finite `TRANSFER_SIZE` represented by `--block-count`.

- [ ] **Step 4: Preserve the compiled header exactly**

  Delete `_rewrite_ascii_header` from the GPU producer path. Pass `plan.artifact_files["unpacked.header"]` directly to the writer. The writer copies exactly 4,096 bytes into the next header block and does not parse or set `BYTES_PER_SECOND`, `FILE_SIZE`, `TRANSFER_SIZE`, `CONFIG_ID`, `GEOMETRY_ID`, memory location or processing metadata.

- [ ] **Step 5: Implement smooth complete-block pacing**

  Use `clock_gettime(CLOCK_MONOTONIC)` and absolute `clock_nanosleep` deadlines. For block index `n`, schedule against `start + n/72 seconds`; never accumulate the previous block's execution delay. Open and commit exactly one 52,428,800-byte PSRDADA block per deadline, retain a deterministic fill pattern, and emit JSON with scheduled/written blocks, bytes, elapsed time, effective Gbps, late blocks and maximum lateness.

- [ ] **Step 6: Run regression**

  Run `gpu_pressure_plan_test`, `python3 -B tests/task8c_rate_point_test.py -q` and `git diff --check`. Require the ring header observed by `pipeline_worker` to satisfy its unchanged exact metadata contract.

### Task 2: Correct the GPU active-throughput timing boundary

**Files:**
- Modify: `include/rdma_dada/pipeline/worker_metrics.h`
- Modify: `src/pipeline/worker_metrics.cpp`
- Modify: `src/pipeline/gpu_block_pipeline.cpp`
- Modify: `scripts/task8c_rate_point.py`
- Test: `tests/pipeline_worker_metrics_test.cpp`
- Test: `tests/task8c_rate_point_test.py`

**Interfaces:**
- Consumes: staged block submission and ordered publication events.
- Produces: `active_elapsed_ns` and `active_input_payload_gbps`, measured from first block submission to last ordered publication. Existing `transfer_elapsed_ns` remains lifecycle diagnostics and is not an acceptance throughput.

- [ ] **Step 1: Add a failing metrics test**

  Record an artificial first submission at 1,000 ns and final publication at 5,001,000 ns for two blocks totalling 1,048,576 B. Assert `active_elapsed_ns=5,000,000` and `active_input_payload_gbps=1.6777216`, independently of a larger lifecycle `transfer_elapsed_ns`.

- [ ] **Step 2: Run the test and verify RED**

  Run `cmake --build build-gpu-budget-local --target pipeline_worker_metrics_test && ./build-gpu-budget-local/pipeline_worker_metrics_test`.

  Expected: failure because the active interval fields do not yet exist.

- [ ] **Step 3: Implement the active interval**

  Add monotonic first-submission and last-publication timestamps to `WorkerMetrics`. The staged pipeline supplies timestamps at the actual enqueue boundary and after ordered output publication. Emit both active fields in metrics JSON. Do not redefine `transfer_elapsed_ns`.

- [ ] **Step 4: Make controller acceptance use the correct field**

  In GPU-only statistics, require `active_elapsed_ns > 0` and `active_input_payload_gbps > 0`; retain block/byte/ring-registration checks. Remove `input_payload_gbps` based on lifecycle elapsed from the performance verdict while retaining it as diagnostic data.

- [ ] **Step 5: Run local regression**

  Run `python3 -B tests/task8c_rate_point_test.py -q`, `./build-gpu-budget-local/pipeline_worker_metrics_test`, and `git diff --check`.

  Expected: all tests pass; the validator rejects zero/absent active intervals for a performance run.

### Task 3: Build and preflight the 30 Gbps GPU-only topology

**Files:**
- Use: `config/testing/atfp-throughput-observation-staged.json`
- Use: `scripts/task8c_rate_point.py`
- Produce remotely: `/home/user/wy/build-cuda-register-30g-20260828`
- Produce remotely: `/home/user/wy/task8c-gpu-30g-results`

**Interfaces:**
- Consumes: canonical synchronized source, strict producer/header contract from Task 1 and corrected metrics from Task 2.
- Produces: verified binaries and a no-resource GPU-only preflight artifact.

- [ ] **Step 1: Run Phase 0 before any build or resource creation**

  Verify qths1 CMake/CTest 3.31.12, CUDA 12.8, RTX 3090, PSRDADA absolute tools, `dada_cuda.h`, NUMA/CPU availability, GPU free memory, and absence of task-owned rings/processes.

- [ ] **Step 2: Synchronize and verify identity**

  Incrementally mirror the complete local source tree to the four canonical directories, preserving build/results/data exclusions, then run the complete manifest check on every host.

- [ ] **Step 3: Build fresh CUDA Release targets**

  Configure `/home/user/wy/build-cuda-register-30g-20260828` with CUDA 12.8, SM86, Release, PSRDADA prefix and testing enabled. Build `observation_config_compile`, `pipeline_worker`, `pipeline_worker_metrics_test`, `gpu_block_pipeline_interface_test`, `gpu_block_pipeline_cuda_test`, `pipeline_worker_core_test`, `pipeline_worker_cuda_chain_test`, and `pipeline_worker_cuda_products_test`.

- [ ] **Step 4: Run affected correctness gates three times**

  Run the registered controller, metrics, interface, CUDA pipeline, worker core, chain and products tests with `ctest --repeat until-fail:3`. Require ring CUDA registration, zero input staging, bounded three-slot lifecycle, exact output order and numerical results.

- [ ] **Step 5: Run a no-resource GPU-only preflight**

  From HF, execute the canonical runner with `--pipeline-stage gpu --aggregate-gbps 30 --duration-seconds 30 --compute-consumer dbnull --gpu-worker-cpu 21 --sink-cpu-list 20 --numa-node 1 --experiment-name bootstrap-gpu-v1 --preflight-only`, using the fresh compiler/build and staged observation JSON.

  Require compute ring `00d4` = 8 × 52,428,800 B, output ring `00d6` = 8 × 819,200 B, staged inflight blocks = 3, block rate = 72/s, total blocks = 2,160, actual input = 30.1989888 Gbps, and no receiver/unpack/sender roles.

### Task 4: Run the single-GPU 30 Gbps acceptance

**Files:**
- Use: `scripts/task8c_rate_point.py`
- Produce remotely: `/home/user/wy/task8c-gpu-30g-results`; the controller records the exact generated suite path in its result callback

**Interfaces:**
- Consumes: Task 3 preflight and fresh binaries.
- Produces: isolated single-GPU capacity evidence without network receive/unpack variables.

- [ ] **Step 1: Run one 30-second diagnostic**

  Execute the same GPU-only request with `--warmup-runs 0 --measured-runs 1 --execute`. Stop if the input writer cannot sustain 72 complete blocks/s, if any block count fails to close, or if cleanup fails.

- [ ] **Step 2: Check the diagnostic acceptance fields**

  Require `input_ring_cuda_registered=true`, 8 registered blocks/419,430,400 B, positive one-time registration duration, input staging bytes/copy time = 0, exactly 2,160 submitted/completed/published blocks, output bytes = 1,769,472,000, bounded `max_inflight` in 1..3, ordered publication, clean output-ring EOD and `dada_dbnull` exit 0. Report producer elapsed separately from GPU active elapsed.

- [ ] **Step 3: Run formal repeatability only after the diagnostic passes**

  Run a fresh 60-second preflight, then execute the 30 Gbps GPU-only request with `--duration-seconds 60 --warmup-runs 1 --measured-runs 3 --execute`. Each run contains 4,320 input blocks and 3,538,944,000 output bytes. Require all four runs PASS/CLEANUP PASS. Record actual producer rate, active GPU throughput, H2D/algorithm/D2H totals and means, slot/writer waits, max inflight, GPU memory budget and per-run identities.

- [ ] **Step 4: Promote the GPU profile only after validation**

  Import the compact suite into the result catalog and promote an immutable qths1 GPU 30 Gbps profile only if the suite manifest, evidence hashes and all repetitions validate.

### Task 5: Run the full-chain 30 Gbps diagnostic and acceptance

**Files:**
- Use: `config/testing/profiles/qths1-unpack-30gbps-60s-v1.json`
- Use: `config/testing/atfp-throughput-observation-staged.json`
- Use: `scripts/task8c_rate_point.py`
- Produce remotely: `/home/user/wy/task8c-full-30g-results`

**Interfaces:**
- Consumes: accepted unpack baseline and passing GPU-only profile from Task 4.
- Produces: complete UDP → raw ring → ATFP compute ring → GPU → output ring sustained evidence.

- [ ] **Step 1: Verify the full preflight has no baseline drift**

  Run full-stage preflight at 30 Gbps with `--baseline-profile config/testing/profiles/qths1-unpack-30gbps-60s-v1.json` and a named experiment that adds GPU/output roles only. Require ports 45871/55871, preparation 1 s, CPU roles 13/14–19/21/20, NUMA1, poll32, WR1024 and accepted raw/compute geometry. The diff may add GPU mode/inflight/output geometry but may not alter ingress/unpack fields.

- [ ] **Step 2: Run one 30-second full diagnostic**

  Execute warm-up 0 + measured 1. Require both Station senders to start; if either fails, abort the whole observation. Stop after this run on any sender, receiver, unpack, GPU, output, EOD or cleanup failure.

- [ ] **Step 3: Verify end-to-end closure**

  Require exact sender scheduled/sent counts, receiver accepted=published, unpack Station and group closure with zero missing/late/duplicate/header errors, CUDA ring registration and zero input staging, GPU submitted=completed=published, exact output bytes, `dada_dbnull` EOD and scoped cleanup. Identify the first divergent boundary from machine-readable counters; do not infer it from aggregate sender rate.

- [ ] **Step 4: Run formal full acceptance only after the diagnostic passes**

  Run 60-second full topology with one warm-up plus three measured repetitions. All repetitions must PASS/CLEANUP PASS. Do not rerun standalone receiver/unpack suites before or between repetitions.

- [ ] **Step 5: Archive compact evidence and update the catalog**

  Retain only `summary.json`, `preflight.json`, observation/resolved identity, `runs/*.json`, one immutable evidence log per run, suite manifest, and first-failure debug files when applicable. Import the suite into the result catalog; promote it only after all formal repetitions pass.

### Task 6: Documentation and Git handoff

**Files:**
- Modify after tests: `docs/PROJECT_STATUS.md`
- Modify after tests: `docs/agents/testing.md`
- Modify after tests: `tests/README.md`

**Interfaces:**
- Consumes: authoritative compact GPU-only and full-chain result roots.
- Produces: status statements with exact evidence boundaries.

- [ ] **Step 1: Record the observed result without overclaiming**

  State separately whether GPU-only 30 Gbps and full-chain 30 Gbps passed. Bind every claim to its exact suite path, repetitions, source/config/binary SHA and cleanup result. A failed full run does not invalidate a passing GPU-only result.

- [ ] **Step 2: Run documentation consistency checks**

  Regenerate the complete source manifest, run its generator tests, and run `git diff --check`.

- [ ] **Step 3: Ask the user before committing**

  Report the tested file set and results. Do not create a Git commit or push until the user explicitly authorizes it.
