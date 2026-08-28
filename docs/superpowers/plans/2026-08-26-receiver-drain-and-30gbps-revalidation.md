# Receiver Drain and 30 Gbps Revalidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Verify the one-second receiver drain without changing the accepted runtime baseline, then re-establish repeatable 30 Gbps receive-only and receive-plus-unpack evidence.

**Architecture:** Keep `rdma2dada`, `vdif_unpack_worker`, their PSRDADA process boundaries and the existing two-Station UDP topology unchanged. Treat the fixed one-second drain as the only product variable: the receiver continues CQ processing and WR repost during drain, while the controller records three aggregate fields. Use the historical passing CPU/NUMA, queue, ring, window and preparation settings as immutable baseline profiles; run receive-only first and permit unpack only after receiver closure passes.

**Tech Stack:** C++11, libibverbs RAW_PACKET, PSRDADA, Python 3 versioned controller, CMake/CTest, JSON/SHA256 test artifacts.

**Spec:** `docs/superpowers/specs/2026-08-08-atfp-unpack-transpose-design.md`

## Global Constraints

- HF is the orchestration host; qths1 is the only PSRDADA/RDMA receiver host; qtpulsar1 and qtpulsar2 are UDP sender hosts.
- Run Phase 0 before synchronization or test implementation and use the observed absolute paths and supported options.
- Test hosts do not run Git. The local development worktree is the source of truth and is verified remotely with `config/testing/atfp-throughput-source-manifest.sha256`.
- Preserve the last passing placement: receiver poll CPU 13, unpack roles CPUs 14–19, sink CPU 20 and NUMA node 1, but first confirm these CPUs still belong to the intended NUMA node.
- Preserve receiver poll batch 32, receive WR depth 1024, raw ring 16 blocks of 52,838,400 bytes, compute ring 8 blocks of 52,428,800 bytes, unpack window 31 blocks/198,400 groups, reorder horizon 96,000 groups and one-second unpack preparation lead.
- Use one recorded source-port pair for the complete warm-up plus three-measured suite. A repetition must not derive a new port from `warmup-01` or `measured-N`; changing the pair is a separately named flow-hash experiment.
- A formal performance result is one warm-up followed by three measured runs. Stop at the first failure and retain only the compact JSON/evidence contract.
- Do not run GPU worker or full-pipeline tests in this plan.
- Do not commit or push automatically. After server PASS, report that the changes are ready and let the user decide.

---

### Task 1: Phase 0 and Historical Baseline Audit

**Files:**
- Read: `docs/agents/testing.md`
- Read: `docs/VDIF_UNPACK_STATUS.md`
- Read: `config/testing/profiles/README.md`
- Read: retained qths1 result directories for the latest exact 30 Gbps receive/unpack PASS

**Interfaces:**
- Consumes: HF/qths/qtp connectivity and retained machine-readable result artifacts.
- Produces: one read-only Phase 0 report and exact profile source evidence; creates no ring or process.

- [ ] **Step 1: Run read-only Phase 0 from HF**

Record connectivity, UTC, absolute CMake/CTest 3.31.12 paths, GCC, Python, PSRDADA binaries/libraries, qths1 NIC/link/NUMA/CPU/memory state, qtp sender prerequisites and existing task-owned processes/IPC.

- [ ] **Step 2: Verify no stale resources**

Require no `rdma2dada`, `vdif_unpack_worker`, `pipeline_worker`, `dada_db`, `dada_dbnull` or `fpga_sender_sim` owned by an unfinished test and no collision on planned ring keys. If ownership is unclear, stop with `ENV_BLOCKED`; do not delete it.

- [ ] **Step 3: Audit retained passing configurations**

Read only the authoritative suite/run JSON from the latest exact-closing 30 Gbps runs. Extract CPU/NUMA placement, poll/WR settings, ring/window geometry, preparation policy, duration, source ports, source/config/binary SHA and exact result paths. Do not reconstruct a profile from chat summaries.

- [ ] **Step 4: Compare historical identities**

Report every difference between the historical PASS and the current requested drain build. The expected product difference is receiver drain behavior and its three result fields; any additional runtime configuration difference blocks the matched comparison.

---

### Task 2: Freeze Receive and Unpack Baseline Profiles

**Files:**
- Create: `config/testing/profiles/qths1-receive-30gbps-v1.json`
- Create: `config/testing/profiles/qths1-unpack-30gbps-v1.json`
- Test: `tests/task8c_profiles_test.py`
- Modify: `config/testing/atfp-throughput-source-manifest.sha256`

**Interfaces:**
- Consumes: exact PASS run evidence from Task 1 and `config/testing/profile.schema.json`.
- Produces: immutable profiles accepted by `task8c_profiles.load_profile()` and used by every formal command below.

- [ ] **Step 1: Write profile-validation tests**

Add fixtures that load both qths1 profiles and assert the exact topology-specific fields, evidence path/SHA, placement, queue geometry, ring/window geometry and preparation policy. Reject scheduler-default or missing evidence fields.

- [ ] **Step 2: Run the tests and verify RED**

Run:

```bash
python3 -B tests/task8c_profiles_test.py -q
```

Expected: failure because the two profile files do not exist.

- [ ] **Step 3: Create profiles from retained PASS artifacts**

Populate the two JSON files only from Task 1 artifacts. `receive` contains receiver/sink/NUMA and raw-ring settings. `unpack` additionally contains ordered unpack CPUs 14–19, compute-ring, window, reorder-horizon and one-second preparation lead.

- [ ] **Step 4: Run profile and controller tests**

Run:

```bash
python3 -B tests/task8c_profiles_test.py -q
python3 -B tests/task8c_rate_point_test.py -q
```

Expected: both exit 0.

- [ ] **Step 5: Regenerate and verify the source manifest**

Run:

```bash
python3 scripts/generate_source_manifest.py --root . --output config/testing/atfp-throughput-source-manifest.sha256
shasum -a 256 -c config/testing/atfp-throughput-source-manifest.sha256
```

Expected: every entry reports OK or the quiet verification exits 0.

---

### Task 3: Freeze Sender Endpoints at Suite Scope

**Files:**
- Modify: `scripts/task8c_rate_point.py`
- Test: `tests/task8c_rate_point_test.py`

**Interfaces:**
- Consumes: the suite-level `observation_id` already generated by `observation_id_for_run_directory()`.
- Produces: one deterministic `(qtpulsar1 source port, qtpulsar2 source port)` pair shared by warm-up and every measured repetition and recorded in `preflight.json`/run process evidence.

- [ ] **Step 1: Replace the existing per-run expectation with a RED suite-level test**

```python
def test_sender_source_ports_are_fixed_for_one_suite():
    suite = pathlib.Path("/results/unpack-30g-suite")
    identities = [
        observation_id_for_run_directory(suite / "warmup-01"),
        observation_id_for_run_directory(suite / "measured-01"),
        observation_id_for_run_directory(suite / "measured-02"),
        observation_id_for_run_directory(suite / "measured-03"),
    ]
    pairs = [derive_sender_source_ports(identity) for identity in identities]
    assert len(set(pairs)) == 1
    assert pairs[0][0] != pairs[0][1]
```

- [ ] **Step 2: Run the controller test and verify RED**

Run:

```bash
python3 -B tests/task8c_rate_point_test.py -q
```

Expected: the existing per-run source-port behavior conflicts with the new suite-level assertion or the bundle still records different endpoint pairs.

- [ ] **Step 3: Derive sender specifications from the compiled suite identity**

In `SshBackend._write_bundle`, pass the compiled
`plan.source["observation"]["observation_id"]` to
`sender_specs_for_plan()` instead of `self.remote_run_dir`. Keep the remote
directory run-specific for cleanup ownership; only endpoint identity becomes
suite-scoped.

- [ ] **Step 4: Add persisted-evidence assertions**

Extend the fake-backend suite test to assert all four run manifests contain the
same two source ports and that `sender-processes.json` still records host,
Station, endpoint, PID/PGID, start/end UTC and return code independently for
every repetition.

- [ ] **Step 5: Run controller regression three times**

Run:

```bash
for run in 1 2 3; do python3 -B tests/task8c_rate_point_test.py -q || exit 1; done
```

Expected: all repetitions exit 0 and no project-root `sender-processes.json`
or task-owned resource remains.

---

### Task 4: Linux Build and Drain Regression Gates

**Files:**
- Verify: `include/rdma_dada/io/rdma/receive_policy.h`
- Verify: `include/rdma_dada/io/rdma/receiver.h`
- Verify: `src/io/rdma/receive_policy.cpp`
- Verify: `src/io/rdma/receiver.cpp`
- Verify: `scripts/task8c_rate_point.py`
- Test: `tests/rdma_receive_policy_test.cpp`
- Test: `tests/task8c_rate_point_test.py`

**Interfaces:**
- Consumes: synchronized manifest, validated qths1 toolchain and Task 3 fixed endpoint behavior.
- Produces: fresh Release receiver/sender binaries and three clean regression repetitions.

- [ ] **Step 1: Mirror and verify all hosts**

Mirror the complete local source tree through HF with standard exclusions. Verify the full source manifest on HF, qths1, qtpulsar1 and qtpulsar2 before configure.

- [ ] **Step 2: Build fresh Release trees**

On qths1 configure `/home/user/wy/rdma-receiver-drain-release-20260826` with the validated CMake, `BUILD_RDMA_PIPELINE=ON`, `USE_CUDA=OFF`, `CMAKE_BUILD_TYPE=Release`, and explicit PSRDADA prefix/pkg-config paths. Build `rdma2dada`, `vdif_unpack_worker`, `rdma_receive_policy_test` and controller tests. Build `fpga_sender_sim` independently on each qtp host without PSRDADA/CUDA and verify identical sender binary SHA.

- [ ] **Step 3: Run portable and controller regression three times**

Run `rdma_receive_policy_test` and `tests/task8c_rate_point_test.py -q` three consecutive times on qths1. Require the drain policy assertions and parsing of `drain_duration_ns`, `completions_after_stop`, and `exit_reason` in every repetition.

- [ ] **Step 4: Verify Release identity**

Record compiler flags, executable SHA256 and `ldd`; reject a stale/default build directory or missing `-O3 -DNDEBUG`.

---

### Task 5: Low-Rate Receiver Drain Functional Acceptance

**Files:**
- Execute: `scripts/task8c_rate_point.py`
- Consume profile: `config/testing/profiles/qths1-receive-30gbps-v1.json`
- Retain: compact suite `summary.json`, `preflight.json`, `runs/*.json`, `runs/*.evidence.log`, `MANIFEST.sha256`

**Interfaces:**
- Consumes: qths1 Release binaries, two sender binaries and the receive baseline profile.
- Produces: a low-rate proof that drain/repost/EOD works before performance testing.

- [ ] **Step 1: Run profile-backed preflight at 0.1 Gbps**

Use `--pipeline-stage receive --compute-consumer dbnull --aggregate-gbps 0.1 --duration-seconds 10 --baseline-profile config/testing/profiles/qths1-receive-30gbps-v1.json --preflight-only` with explicit qths/sender build directories, observation JSON and compiler.

- [ ] **Step 2: Execute one warm-up plus three measured runs**

Use the same arguments with `--warmup-runs 1 --measured-runs 3 --execute` and a topology-scoped result root under `/home/user/wy/task8c-receiver-drain-results`.

- [ ] **Step 3: Validate drain semantics**

For every run require sender scheduled=sent, receiver accepted=published=expected, `wrong_length=0`, `cq_errors=0`, `repost_failures=0`, `exit_reason=DRAIN_DEADLINE`, `drain_duration_ns >= 1000000000`, clean raw-ring EOD and `CLEANUP_RESULT=PASS`. Record `completions_after_stop` without requiring it to be nonzero.

- [ ] **Step 4: Stop on any mismatch**

If drain duration is below one second, WR repost stops during drain, counts fail to close, or cleanup leaves resources, classify the first failing stage and return the compact evidence before attempting 30 Gbps.

---

### Task 6: Receive-Only 30 Gbps Repeatability Gate

**Files:**
- Execute: `scripts/task8c_rate_point.py`
- Consume profile: `config/testing/profiles/qths1-receive-30gbps-v1.json`

**Interfaces:**
- Consumes: Task 5 PASS and unchanged receiver baseline.
- Produces: authoritative receiver/NIC admission evidence at 30 Gbps.

- [ ] **Step 1: Run profile-backed 30 Gbps preflight**

Set `--aggregate-gbps 30 --duration-seconds 60 --pipeline-stage receive --compute-consumer dbnull`. Require the profile diff to be empty before creating rings.

- [ ] **Step 2: Execute one warm-up plus three measured runs**

Require both Stations to complete their schedules and keep the same suite-scoped source-port pair across warm-up/measured repetitions.

- [ ] **Step 3: Validate every measured run**

Require sender total=receiver accepted=published, zero PHY/admission deficit, zero CQ/repost/wrong-length errors, `exit_reason=DRAIN_DEADLINE`, drain duration at least one second, clean raw dbnull EOD and cleanup PASS.

- [ ] **Step 4: Diagnose before proceeding on failure**

Use run-scoped NIC deltas, receiver counts, `completions_after_stop`, CQ/repost metrics and ordinal distribution. A small tail-only deficit implicates stop/drain; loss accumulating during the 60-second interval implicates steady-state admission/service. Do not run unpack after a receive failure.

---

### Task 7: Receive Plus Unpack 30 Gbps Repeatability Gate

**Files:**
- Execute: `scripts/task8c_rate_point.py`
- Consume profile: `config/testing/profiles/qths1-unpack-30gbps-v1.json`

**Interfaces:**
- Consumes: Task 6 PASS and unchanged network/sender geometry.
- Produces: authoritative receive-plus-unpack evidence without GPU worker.

- [ ] **Step 1: Run profile-backed unpack preflight**

Set `--aggregate-gbps 30 --duration-seconds 60 --pipeline-stage unpack --compute-consumer dbnull`. Require the exact CPUs 13–20, NUMA1, queue/ring/window geometry and one-second preparation lead from the profile.

- [ ] **Step 2: Execute one warm-up plus three measured runs**

Use the same source-port pair policy and stop at the first failed repetition.

- [ ] **Step 3: Validate the complete boundary**

Require receiver closure from Task 6 plus `unpack.records=receiver.published`, `complete_groups=EXPECTED_GROUPS`, zero missing/incomplete/late/duplicate/invalid/unknown/out-of-range counters, exact compute bytes, clean compute dbnull EOD and cleanup PASS.

- [ ] **Step 4: Report the first saturated stage**

If receiver still closes but unpack fails, compare raw-ring occupancy, coordinator/parser/writer service times and queue HWM against the block deadline. Do not change ring/window size as a substitute for insufficient steady-state service rate.

---

### Task 8: Decide Whether Expected-Count Early Exit Is Needed

**Files:**
- Potential future modify: `include/rdma_dada/io/rdma/receiver.h`
- Potential future modify: `src/io/rdma/receiver.cpp`
- Potential future modify: `apps/rdma2dada/main.cpp`
- Potential future test: `tests/rdma_receive_policy_test.cpp`

**Interfaces:**
- Consumes: Task 5–7 drain evidence.
- Produces: a documented decision; no behavior change unless separately approved.

- [ ] **Step 1: Compare drain evidence**

If counts close and the only cost is approximately one second of orderly shutdown, retain the fixed drain. It is simpler and keeps VDIF Station/ordinal parsing in unpack.

- [ ] **Step 2: Evaluate a fast path only if justified**

The candidate rule is: after all mandatory senders finish, exit drain early when the receiver has observed the configured physical record count; otherwise retain the one-second deadline. Treat this only as a latency optimization, never as completeness proof, because duplicates can satisfy a total count while an ordinal is missing.

- [ ] **Step 3: Require separate approval and TDD for any fast-path change**

Do not mix that change into the current 30 Gbps comparison. If approved later, add RED tests for exact-count early exit, loss fallback, duplicate ambiguity and continued WR repost before modifying receiver code.

---

### Task 9: Evidence Handoff and Git Decision

**Files:**
- Update after accepted results: `docs/PROJECT_STATUS.md`
- Update after accepted results: `docs/VDIF_UNPACK_STATUS.md`
- Update after accepted results: `docs/agents/testing.md`

**Interfaces:**
- Consumes: compact PASS/FAIL artifacts from Tasks 5–7.
- Produces: current module status with exact suite paths and an explicit Git readiness notice.

- [ ] **Step 1: Record only accepted evidence**

Bind every claim to an exact suite root, repetition count, source/config/binary SHA, `TEST_RESULT` and `CLEANUP_RESULT`. Do not promote a single run to a stable rate.

- [ ] **Step 2: Return results to the development task**

The `GPU服务器代码测试` task sends the complete report to source task `019fbaf1-006f-7970-8566-5d3d51086698`, even if Phase 0, sync, build or the first run fails.

- [ ] **Step 3: Import and index in the development task**

The development task verifies/transfers each compact suite, runs
`task8c_catalog.py import` and `verify`, and maintains the test summary table.
`总结成文` receives no per-test notification and queries this catalog only when
the user requests a paper update.

- [ ] **Step 4: Ask the user for the Git decision**

After explicit server PASS, report that the receiver drain changes and baseline profiles are ready for commit. Do not commit or push without user authorization.
