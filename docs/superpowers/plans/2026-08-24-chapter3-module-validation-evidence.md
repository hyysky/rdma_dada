# Chapter 3 Module Validation Evidence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce submission-grade, reproducible module-level functional, numerical, performance, and controlled-optimization evidence for paper Chapter 3 without mixing in Chapter 4 end-to-end claims.

**Architecture:** Keep the existing product boundaries and four-stage performance controller. Add one focused module-validation suite for repeated CTest evidence, extend unpack/controller metrics only where the current JSON cannot support a quantitative conclusion, and use the existing `receive` and `unpack` stages for formal network performance gates. All paper-facing conclusions consume compact suite JSON; raw logs remain audit evidence only.

**Tech Stack:** C++11, CUDA 12.8/SM86, CMake/CTest 3.31.12, Python 3.8+, PSRDADA, libibverbs, JSON, SHA256.

**Spec:** `docs/agents/testing.md`

## Global Constraints

- This plan covers paper Chapter 3 module validation and optimization only.
- Full `rdma2dada -> unpack -> GPU worker -> output` sustained performance, multi-GPU/multi-node scaling, and end-to-end headroom belong to Chapter 4 and are excluded from the Chapter 3 acceptance gate.
- Every server activity starts with the repository Phase 0 read-only preflight and follows the existing `GPU服务器代码测试` handoff workflow.
- Historical evidence may be reused only when its exact suite/run path, source/config/binary identities, command, repetition count, result, cleanup, and artifact manifest can be verified. Conversation summaries are not authoritative evidence.
- Functional/numerical acceptance requires at least three consecutive clean repetitions.
- Performance acceptance requires one warm-up followed by at least three consecutive measured repetitions.
- Paper-facing readers consume only suite-root `summary.json`, `preflight.json`, and `runs/*.json`. `*.evidence.log` proves selected raw lines; `debug/<run-id>/` is consulted only for a failed run.
- No CSV is required or manufactured. Every conclusion binds to an exact retained suite/run path.
- A 30 Gbps finite transfer or one successful repetition remains exploratory evidence.
- Optimization comparisons change one named variable at a time and record the full baseline diff before creating resources.
- The current public performance topologies remain exactly `receive`, `unpack`, `gpu`, and `full`; this plan does not add a synthetic unpack topology.

---

## Current Coverage Audit

| Chapter 3 module family | Reusable implementation/tests | Evidence gap before paper acceptance |
| --- | --- | --- |
| Observation/config identity/Project VDIF | `observation_config_test`, `resolved_observation_plan_test`, `config_identity_test`, `packet_format_config_test`, `project_vdif_v1_test`, `observation_artifacts_test`, `observation_config_compile_test` | Existing tests are individually useful, but need one three-repetition suite with exact binary/config identity and compact JSON output |
| RDMA receive/raw-ring boundary | `rdma_receive_policy_test`; controller `receive` stage; 30 Gbps/60 s warm-up+3 formal PASS with CQ/repost/accepted/published counters | Formal admission baseline exists; Chapter 3 still needs the module-suite packaging and paper-facing extraction |
| Parallel VDIF unpack | config/header/timeline/engine/ATFP/writer unit tests, `vdif_unpack_worker_integration_test`; 30 Gbps/60 s warm-up+3 formal PASS | Need three-repetition module-suite packaging plus parse/copy/raw-block service timing, process CPU usage and worker-count comparison |
| GPU format/beam/product/integration/transfer | CPU and CUDA tests for conversion, Beamform, Power, Stokes, integration and transfer; CUDA chain/product tests | Need one authoritative three-repetition module suite. Stokes evidence covers exactly `AA`, `BB`, `AB_REAL` and `AB_IMAG`; materialized or derived I/Q/U/V is outside the current engineering and paper scope |
| Result artifacts | `task8c_artifacts.py`, suite `preflight.json`, `summary.json`, `runs/*.json`, evidence logs and first-failure debug contract | CTest module tests do not yet have an equivalent compact suite controller; unpack comparison metrics are not yet complete |
| Matched optimization comparison | versioned profiles, named experiments, worker CPU list, writer queue HWM/waits | No accepted worker-count 1/2/4 comparison exists. A one-worker coordinator/worker/writer run is not a serial-reference implementation and must be described as worker-count scaling |

## File Structure

- Create `config/testing/chapter3-module-suite.json`: exact test groups and required repetition count for Chapter 3.
- Create `scripts/chapter3_module_validation.py`: run registered CTest cases, validate identities, and produce the compact Chapter 3 suite.
- Create `tests/chapter3_module_validation_test.py`: fake-CTest tests for repetition, first-failure stop, evidence extraction, manifests, and cleanup status.
- Modify `CMakeLists.txt`: register the module-suite controller self-test only; product tests remain registered individually.
- Modify `include/rdma_dada/modules/vdif_unpack/vdif_atfp_engine.h`: add raw-block parse/copy/service timing counters.
- Modify `modules/vdif_unpack/vdif_atfp_engine.cpp`: measure coordinator-visible phase wall times without per-record logging.
- Modify `apps/vdif_unpack_worker/main.cpp`: emit the new counters in the existing final statistics line.
- Modify `tests/vdif_atfp_engine_test.cpp`: verify timing counters are present, monotonic, and reset per transfer without asserting machine-specific durations.
- Modify `scripts/task8c_rate_point.py`: parse the new unpack counters, retain process CPU accounting, and expose matched-comparison fields in run JSON.
- Modify `tests/task8c_rate_point_test.py`: verify structured unpack timing, process CPU accounting, profile-diff isolation, and compaction.
- Modify `docs/agents/testing.md` and `tests/README.md`: document the Chapter 3 suite and optimization comparison contract.
- Update `docs/PROJECT_STATUS.md` and the paper-facing evidence ledger only after accepted artifacts exist.

---

### Task 1: Freeze the Chapter 3 Evidence Matrix

**Files:**
- Create: `config/testing/chapter3-module-suite.json`
- Create: `tests/chapter3_module_validation_test.py`
- Create: `scripts/chapter3_module_validation.py`

**Interfaces:**
- Consumes: a fresh CMake build directory, CTest executable path, source manifest, Observation JSON, packet-format JSON, and result root.
- Produces: a suite containing `preflight.json`, `summary.json`, `runs/*.json`, `runs/*.evidence.log`, and `MANIFEST.sha256`.

- [ ] **Step 1: Add the exact module inventory**

Define four groups in `config/testing/chapter3-module-suite.json`:

```json
{
  "schema_version": 1,
  "repetitions": 3,
  "groups": {
    "configuration": [
      "project_vdif_v1_test",
      "packet_format_config_test",
      "observation_config_test",
      "resolved_observation_plan_test",
      "config_identity_test",
      "observation_artifacts_test",
      "observation_config_compile_test"
    ],
    "receive": ["rdma_receive_policy_test"],
    "unpack": [
      "vdif_unpack_config_test",
      "vdif_unpack_header_test",
      "vdif_timeline_test",
      "vdif_unpack_engine_test",
      "vdif_atfp_engine_test",
      "atfp_block_writer_test",
      "vdif_unpack_worker_integration_test"
    ],
    "gpu": [
      "complex_convert_module_test",
      "complex_convert_cuda_test",
      "beamform_module_test",
      "beamform_cuda_fp32_test",
      "beamform_cuda_tf32_test",
      "power_module_test",
      "power_cuda_test",
      "stokes_module_test",
      "stokes_cuda_test",
      "time_integrate_module_test",
      "time_integrate_cuda_test",
      "transfer_cuda_roundtrip_test",
      "pipeline_worker_cuda_chain_test",
      "pipeline_worker_cuda_products_test"
    ]
  }
}
```

- [ ] **Step 2: Write failing controller tests**

Use a fake CTest executable to assert:

```python
self.assertEqual(summary["required_repetitions"], 3)
self.assertEqual(summary["completed_repetitions"], 3)
self.assertEqual(summary["TEST_RESULT"], "PASS")
self.assertEqual(run["source_manifest_sha256"], "a" * 64)
self.assertIn("binary_sha256", run["tests"][0])
self.assertEqual(run["cleanup"]["CLEANUP_RESULT"], "PASS")
```

Also assert that the first failed test stops later repetitions, preserves only its compact evidence/debug files, and never rewrites `TEST_RESULT` during cleanup.

- [ ] **Step 3: Implement the compact module-suite controller**

Require explicit `--ctest`, `--build-dir`, `--suite-config`, `--source-manifest`, and `--result-root`. Use `ctest --test-dir BUILD --show-only=json-v1` to resolve registered commands before execution. Hash the CTest executable, every selected test binary/script, the suite config, Observation config, packet profile, and source manifest. Run every group in each repetition with `ctest --output-on-failure -R` and store per-test command, start/end UTC, return code and classification.

- [ ] **Step 4: Use the existing compact artifact contract**

Reuse `scripts/task8c_artifacts.py` for manifest generation and process-ledger validation where applicable. Keep only the documented suite JSON, one evidence log per repetition, and first-failure debug files.

- [ ] **Step 5: Register and run local controller tests**

Run:

```bash
python3 -B tests/chapter3_module_validation_test.py -q
python3 -B tests/task8c_artifacts_test.py -q
git diff --check
```

Expected: all tests pass without creating a PSRDADA ring, network process, or CUDA context.

### Task 2: Close Stokes Correlation and GPU Numerical Evidence Semantics

**Files:**
- Modify: `tests/stokes_module_test.cpp`
- Modify: `tests/stokes_cuda_test.cpp`
- Modify: `tests/pipeline_worker_cuda_products_test.cpp`
- Modify: `doc/ALGORITHM_MODULE_CONTRACTS.md`

**Interfaces:**
- Consumes: two complex polarization signals per T/F/B sample.
- Produces: exactly the correlations `AA`, `BB`, `Re(A B*)`, `Im(A B*)`.

- [ ] **Step 1: Audit current assertions**

Confirm the existing expected arrays independently evaluate only:

```text
AA = |A|^2
BB = |B|^2
AB_RE = Re(A * conj(B))
AB_IM = Im(A * conj(B))
```

Keep the product format unchanged. I/Q/U/V materialization or derivation is not part of this plan and must not be added to the implementation, tests, result schema or paper claims.

- [ ] **Step 2: Add asymmetric known vectors**

Use nonzero real/imaginary values whose `AB_RE` and `AB_IM` are both nonzero so conjugation and the sign of `AB_IM` cannot accidentally pass.

- [ ] **Step 3: Run portable tests locally**

Build and run the CPU tests on macOS. CUDA acceptance remains deferred to the GPU server task after HF recovery.

### Task 3: Add Unpack Critical-Path Metrics

**Files:**
- Modify: `include/rdma_dada/modules/vdif_unpack/vdif_atfp_engine.h`
- Modify: `modules/vdif_unpack/vdif_atfp_engine.cpp`
- Modify: `apps/vdif_unpack_worker/main.cpp`
- Modify: `tests/vdif_atfp_engine_test.cpp`
- Modify: `scripts/task8c_rate_point.py`
- Modify: `tests/task8c_rate_point_test.py`

**Interfaces:**
- Adds to `VdifAtfpStatistics`:

```cpp
std::uint64_t raw_blocks;
std::uint64_t parse_phase_ns_total;
std::uint64_t parse_phase_ns_max;
std::uint64_t copy_phase_ns_total;
std::uint64_t copy_phase_ns_max;
std::uint64_t raw_block_service_ns_total;
std::uint64_t raw_block_service_ns_max;
```

- [ ] **Step 1: Write reset/aggregation tests**

Assert counters start at zero, increment once per consumed raw block, totals never decrease, maxima do not exceed totals, and a new transfer resets transfer-scoped values.

- [ ] **Step 2: Measure phase wall time**

Measure coordinator-visible wall time around the existing parse dispatch, copy dispatch, and complete `ConsumeRawBlockAsync` critical path. Do not add per-packet clocks, allocation, or logging.

- [ ] **Step 3: Emit and parse structured metrics**

Append stable key/value fields to the existing final unpack statistics line and map them into:

```json
{
  "unpack": {
    "raw_blocks": 0,
    "parse_phase_ns_total": 0,
    "parse_phase_ns_max": 0,
    "copy_phase_ns_total": 0,
    "copy_phase_ns_max": 0,
    "raw_block_service_ns_total": 0,
    "raw_block_service_ns_max": 0,
    "writer_queue_high_watermark": 0,
    "writer_enqueue_wait_ns": 0,
    "writer_acquire_wait_ns": 0
  }
}
```

- [ ] **Step 4: Run local regressions**

Run `vdif_atfp_engine_test`, `atfp_block_writer_test`, and `task8c_rate_point_test.py` three times. Timing values are checked structurally, never against a macOS performance threshold.

### Task 4: Add Process CPU and Ring-Pressure Accounting

**Files:**
- Modify: `scripts/task8c_rate_point.py`
- Modify: `tests/task8c_rate_point_test.py`
- Modify: `scripts/task8c_artifacts.py`
- Modify: `tests/task8c_artifacts_test.py`

**Interfaces:**
- Produces per process: user/system CPU seconds, elapsed seconds, effective CPU cores used, CPU affinity, NUMA binding and thread mapping.
- Produces per ring: maximum observed occupancy, full-sample count, sample count and producer wait/backpressure counters available from the owning process.
- Produces receiver shutdown/drain evidence: expected records, accepted/published at stop request, final accepted/published, last completion time, stop-to-last-completion time, quiet-drain duration, final posted WR count and the reason the poll loop exited.

- [ ] **Step 1: Extend the generated supervisor result**

Use `os.wait4` in the generated qths supervisor and write one atomic JSON lifecycle record containing return code, start/end UTC, elapsed time, `ru_utime`, `ru_stime`, `ru_maxrss`, PID and PGID. Preserve the existing plain exit-code compatibility until all controller tests consume the JSON.

- [ ] **Step 2: Parse existing ring progress without adding hot-path logging**

Parse the existing `rdma2dada` progress lines into raw-ring maximum occupancy/full samples. Combine these with unpack writer acquire/enqueue wait and queue HWM. Do not infer missing samples as zero occupancy.

- [ ] **Step 3: Make finite-transfer drain observable**

Record whether the receiver stopped because it reached the expected finite-transfer record count, encountered an error, or observed its configured quiet-drain condition. Preserve the last CQ completion timestamp and final posted-WR count so a tail-drain race can be distinguished from steady-state NIC admission loss. Poll-count-only quiet periods are diagnostic data, not elapsed-time guarantees.

- [ ] **Step 4: Add fail-closed artifact assertions**

A performance PASS must contain CPU/NUMA/process lifecycle for receiver, unpack worker and sink, plus all topology-required process ledger roles. Missing metrics classify the run as `HARNESS_FAIL`, not `PERFORMANCE_FAIL`.

- [ ] **Step 5: Run controller/artifact tests**

Run the two Python suites three consecutive times and verify success compaction retains the structured fields while removing full progress logs.

### Task 5: Audit Historical Module Evidence After HF Recovery

**Files:**
- Update after evidence is verified: `docs/PROJECT_STATUS.md`
- Update after evidence is verified: paper Chapter 3 evidence ledger maintained by the paper task

**Interfaces:**
- Consumes: exact historical suite roots and their manifests.
- Produces: a reuse/rerun decision for every row in the Current Coverage Audit.

- [ ] **Step 1: Run Phase 0 only after explicit user notification**

Verify HF/qths connectivity, tool paths, CMake/CTest pair, CUDA/GPU, PSRDADA, CPU/NUMA, disk/memory, clock, permissions and absence of task-owned residue. Do not run a test in this step.

- [ ] **Step 2: Inventory exact retained roots read-only**

For each candidate root, verify manifest, source/config/binary SHA, command, repetition count, `TEST_RESULT`, `CLEANUP_RESULT`, and required compact files. Mark it `REUSABLE` or `RERUN_REQUIRED` with a reason. Do not reconstruct missing JSON from prose or logs.

- [ ] **Step 3: Re-run the consolidated module suite where evidence is incomplete**

Mirror the current source by complete manifest, make one fresh CUDA Release build, then run `chapter3_module_validation.py`. Require all four groups to pass three consecutive repetitions.

### Task 6: Establish the Formal Receive Admission Baseline

**Files:**
- Product changes: none before evidence identifies a receiver defect.
- Uses: `scripts/task8c_rate_point.py --pipeline-stage receive`

**Interfaces:**
- Produces a formal receive-only suite with exact sender, NIC, CQ/repost, accepted/published, raw-ring EOD, CPU/NUMA and cleanup accounting.

- [ ] **Step 1: Load the verified receive baseline profile**

Use the latest accepted qths1 receive profile. Any changed queue/WR/CPU/ring value requires a named experiment and exact diff before resource creation.

- [ ] **Step 2: Run 30 Gbps formal acceptance**

Execute 60 seconds, one warm-up and three measured runs. Require scheduled=sent=accepted=published, zero wrong-length/CQ/repost failures, NIC admission reconciliation, clean EOD and cleanup.

If a run has a small deficit, classify its location before retrying: compare sender application counts, sender NIC TX delta, qths physical RX delta, receiver accepted/published, per-second unpack missing distribution, last-completion/stop timestamps and final posted WRs. A deficit concentrated at transfer end is a drain-boundary failure; a deficit distributed through the run is a steady-state admission failure.

- [ ] **Step 3: Establish a lower repeatable baseline if 30 Gbps fails**

Stop the 30 Gbps suite at the first failure. Diagnose the receiver/NIC admission boundary from run JSON. Then run a descending named campaign using 25, 20, 15 and 10 Gbps, stopping at the first rate that passes the full warm-up-plus-three gate. Record 30 Gbps as an unresolved exploratory failure and the lower point as the formal Chapter 3 baseline.

### Task 7: Establish the Formal Unpack Performance Baseline

**Files:**
- Product changes: none before receive Task 6 passes at the selected rate.
- Uses: `scripts/task8c_rate_point.py --pipeline-stage unpack`

**Interfaces:**
- Consumes: the exact accepted receive rate/profile and current unpack profile.
- Produces: repeatable sender→receive→unpack→compute-drain accounting and unpack critical-path metrics.

- [ ] **Step 1: Hold receive conditions fixed**

Use the same sender schedule, duration, receiver poll CPU, WR depth, poll batch, raw geometry, NIC and source/binary identities as the accepted receive suite.

- [ ] **Step 2: Run one warm-up plus three measured runs**

Require receiver exact closure before interpreting unpack. Then require unpack records=receiver published, deterministic complete/missing counts, exact emitted blocks/bytes, clean EOD/cleanup, bounded queue HWM, and no sustained ring-full condition.

- [ ] **Step 3: Treat 30 Gbps honestly**

If the selected formal rate is below 30 Gbps, state that lower rate as the repeatable Chapter 3 baseline and retain 30 Gbps only as exploratory/unresolved evidence.

### Task 8: Run a Matched Worker-Count Comparison

**Files:**
- Uses: `scripts/task8c_rate_point.py --pipeline-stage unpack --experiment-name ...`
- Update after acceptance: `docs/PROJECT_STATUS.md`

**Interfaces:**
- Consumes: one accepted unpack rate/profile and distinct CPUs on the verified NUMA node.
- Produces: worker-count scaling results for 1, 2 and 4 parse/copy workers.

- [ ] **Step 1: Freeze all non-worker variables**

Keep hardware, build, source/config identities, sender input, duration, receiver settings, raw/compute ring geometry, window, reorder horizon, coordinator CPU, writer CPU, sink CPU and NUMA node identical. The only profile diff is the number/list of parser worker CPUs.

- [ ] **Step 2: Run three named experiments**

Use ordered mappings equivalent to:

```text
1 worker: coordinator,worker0,writer
2 workers: coordinator,worker0,worker1,writer
4 workers: coordinator,worker0,worker1,worker2,worker3,writer
```

Each experiment executes one warm-up plus three measured runs at the accepted unpack rate. A receiver count mismatch invalidates that repetition as an unpack comparison.

- [ ] **Step 3: Report quantitative scaling**

For each worker count report median/min/max sustained payload rate, records/s, parse/copy/raw-block service time, receiver/unpack loss accounting, ring occupancy, writer wait/HWM, total unpack CPU utilization and payload-copy/emitted bytes. Calculate service-time ratios only from matched accepted runs.

- [ ] **Step 4: Bound the claim**

Describe the result as `parse/copy worker-count scaling`. Do not call it serial-to-parallel speedup because all three configurations retain coordinator, worker and single-writer stages. If the comparison cannot remain matched, document implementation evolution/optimization rationale only.

### Task 9: Produce the Chapter 3 Paper Handoff

**Files:**
- Update: `docs/PROJECT_STATUS.md`
- Update: exact paper-side evidence ledger through the originating paper task

**Interfaces:**
- Consumes only accepted compact suite artifacts.
- Produces a module-status table, numerical-validation table, unpack performance table, and optimization comparison table with exact evidence paths.

- [ ] **Step 1: Validate every cited suite manifest**

Run manifest verification before extracting a number. Reject a conclusion whose suite/run path or identity is missing.

- [ ] **Step 2: Extract JSON only**

Read `preflight.json`, `summary.json`, and `runs/*.json`. Use evidence logs only to audit selected raw final lines; inspect `debug/` only for explaining a failed result.

- [ ] **Step 3: Preserve the Chapter 3/4 boundary**

Chapter 3 may report module numerical correctness, receive/unpack module performance, worker-count scaling and GPU module tests. It must not claim full-chain sustained rate, multi-GPU scaling, multi-node scaling or end-to-end operating headroom; those remain Chapter 4 work.

## Self-Review

- Spec coverage: all four requested module families, unpack repeatability, controlled optimization, compact artifacts, historical evidence reuse and Chapter 3/4 separation map to Tasks 1–9.
- Interface conflicts: none require a product semantic change. The only planned product instrumentation adds aggregate counters and does not change packet/ring/header behavior.
- Current blocker: HF is unavailable. Tasks 5–9 remain dormant until explicit user notification; Tasks 1–4 are local development work and still require normal GPU-server handoff after completion.
- Claim boundary: worker-count comparison is not a serial-reference speedup; 30 Gbps remains unaccepted until warm-up plus three measured runs pass.
