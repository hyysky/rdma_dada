# Receiver Admission and Full-Pipeline Acceptance Plan

> Execute this plan task by task. Do not commit or push without explicit user authorization.

**Goal:** Establish a repeatable 30 Gbps ingest/unpack boundary, then measure the complete
Resolved-Plan-driven GPU pipeline without conflating receiver loss, unpack throughput, GPU service
rate or sink performance.

**Architecture:** Keep the existing one-QP direct receiver, parallel single-process unpack, one
compute ring, one GPU `pipeline_worker`, one output ring and `dada_dbnull` sink. Extend the versioned
controller and evidence only where current artifacts cannot identify the first saturated stage.

**Primary files:**

- `scripts/task8c_rate_point.py`
- `tests/task8c_rate_point_test.py`
- `scripts/atfp_throughput_campaign.py`
- `tests/atfp_throughput_campaign_test.py`
- `config/testing/atfp-throughput-campaign.json`
- `config/testing/atfp-throughput-observation.json`
- `docs/agents/testing.md`
- `tests/README.md`

**Related contracts:**

- `docs/superpowers/specs/2026-08-19-parallel-vdif-unpack-design.md`
- `docs/superpowers/specs/2026-08-09-atfp-throughput-campaign-controller-design.md`
- `docs/superpowers/specs/2026-08-08-unified-observation-config-design.md`

**Execution note (2026-08-20):** Tasks 2–3 are deferred by user decision and 30 Gbps is used only
as a provisional receive/unpack planning baseline. Task 4 is implemented in the current development
worktree with an explicit performance-rate override, dual-rate provenance, 20% deadline reserve,
combined H2D+D2H demand and checked VRAM planning. RTX 3090 fresh Release build, exact budget
artifact, capacity gate and CUDA regressions passed on 2026-08-20; this is not a 30 Gbps runtime
performance result.

## Global gates

- Formal performance means one warm-up plus three measured repetitions; every measured run passes.
- Use 60-second measured runs for the 30 Gbps receive/unpack gate.
- `receive` stops at raw-ring `dada_dbnull`; `unpack` stops at compute-ring `dada_dbnull`; `full`
  includes `pipeline_worker`, output ring and output `dada_dbnull`.
- Never reuse an unpack-only result as full-pipeline evidence.
- Every run records exact source/binary/config SHA, CPU/NUMA mapping, NIC deltas, process exit/EOD,
  owned ring keys and cleanup separately from product result.
- If a mandatory Station fails to start or exits early, abort the complete observation.
- Stop at the first failed rate point and diagnose before changing ring or window capacity.

---

## Task 1: Freeze the current baseline and evidence schema

**Files:**

- Modify if coverage is absent: `tests/task8c_rate_point_test.py`
- Modify if fields are absent: `scripts/task8c_rate_point.py`
- Update: `docs/agents/testing.md`
- Update: `tests/README.md`

- [ ] Add/confirm controller tests for `pipeline_stage=receive|unpack|full`, required binaries and
  stage-specific ring/process topology.
- [ ] Add/confirm result validation for sender scheduled/sent/failed, receiver accepted/published,
  unpack records/groups/bytes, final output bytes, EOD and cleanup.
- [ ] Require run-scoped NIC before/after snapshots and deltas; a missing or reset counter is explicit
  evidence failure, never interpreted as zero loss.
- [ ] Persist receiver CQ poll calls, batch histogram or useful batch counters, repost failures,
  wrong-length/fatal completion counters and raw-block publish wait.
- [ ] Persist raw/compute/output ring occupancy high-water and sustained-full duration.
- [ ] Run `python3 tests/task8c_rate_point_test.py` three consecutive times.
- [ ] Run `python3 tests/atfp_throughput_campaign_test.py` three consecutive times.
- [ ] Run `git diff --check`.

## Task 2: Receive-only 30 Gbps formal gate

**Product changes:** none before evidence identifies a product defect.

**Versioned entry:** `scripts/task8c_rate_point.py --pipeline-stage receive --compute-consumer dbnull`

- [ ] The GPU server task performs Phase 0 and mirrors the complete current source tree using the
  supplied SHA manifest.
- [ ] Build a fresh Release tree with the validated CMake/CTest pair and record binary/config SHA.
- [ ] Run preflight-only with receiver CPU 13, sink CPU 20, NUMA node 1, poll batch 32 and WR depth
  1024; use observed server topology rather than assuming these values remain valid.
- [ ] Execute 30 Gbps, 60 s, one warm-up plus three measured runs.
- [ ] Require both senders to send the complete schedule, receiver accepted=published=expected,
  zero fatal CQ/wrong-length errors, clean raw EOD and clean scoped cleanup in every measured run.
- [ ] Compare NIC packet/drop/steering deltas with receiver counts and identify the admission boundary.
- [ ] If any run loses packets, stop here. Diagnose NIC steering, CQ service, WR availability and
  block-publish wait before modifying unpack.

**Completion:** receive-only 30 Gbps passes all three measured repetitions, or the first failing
receiver/NIC mechanism is supported by run-scoped evidence.

## Task 3: Unpack-only 30 Gbps formal gate

**Versioned entry:** `scripts/task8c_rate_point.py --pipeline-stage unpack --compute-consumer dbnull`

- [ ] Use the same sender geometry, duration, receiver settings and source/binary SHA as Task 2.
- [ ] Use explicit NUMA1 mapping: receiver poll, coordinator, workers, writer and sink on distinct
  verified CPUs.
- [ ] Execute one warm-up plus three 60-second measured runs.
- [ ] Require unpack records=receiver published, complete/missing/late/duplicate/invalid counters to
  match the deterministic input, exact compute bytes, clean dbnull EOD and cleanup.
- [ ] Require no sustained raw-ring full condition and compare parser/copy/writer p99 with the raw
  block arrival deadline.
- [ ] If Task 2 passes but Task 3 fails, optimize only the measured unpack substage and rerun both
  correctness and performance gates.

**Completion:** 30 Gbps is promoted from “single exploratory closure” to “repeatable unpack-only
stable rate,” with the evidence boundary stated explicitly.

## Task 4: Compile a full-pipeline feasibility budget

**Files:**

- Modify: `src/config/observation_artifacts.cpp`
- Modify: `tests/observation_artifacts_test.cpp`
- Modify: `tools/observation_config_compile/main.cpp` only if a report field needs exposure
- Update: `doc/ALGORITHM_MODULE_CONTRACTS.md`

- [x] Use Resolved Plan ring/block geometry and explicit payload-rate provenance as the budget input.
- [x] Calculate compute-block arrival interval and the full GPU service deadline.
- [x] Calculate ATFP CI8 input, TFPA CF32 converted buffer, FPAB weights, TFPB beam output,
  Power/TFBS Stokes output and integrated output bytes with checked arithmetic.
- [x] Report minimum H2D, kernel-chain and D2H throughput required to keep up, plus selected operating
  headroom. Use 20% headroom as the initial engineering gate unless hardware evidence justifies a
  different recorded value.
- [x] Report combined sequential H2D+D2H volume and planned/recommended-free VRAM, while explicitly
  excluding CUDA runtime/library workspace.
- [x] Reject configurations whose output geometry, block scaling or memory requirement is invalid;
  do not silently shrink T or change product format.
- [x] Add unit tests for Beamformed, Power, Stokes and integrated outputs, including overflow and
  non-divisible integration cases.
- [x] Verify the exact 30 Gbps artifact and CUDA regressions on RTX 3090 in a fresh Release build.

**Completion:** passed for the initial budget/capacity scope. The compiler states required memory and
the per-block service deadline; the server preflight decides whether current free VRAM fits. Runtime
performance remains a separate Task 6–7 gate.

## Task 5: Close the full-stage controller correctness gaps

**Files:**

- Modify: `scripts/task8c_rate_point.py`
- Modify: `tests/task8c_rate_point_test.py`
- Modify: `scripts/atfp_throughput_campaign.py`
- Modify: `tests/atfp_throughput_campaign_test.py`
- Update: `docs/agents/testing.md`
- Update: `tests/README.md`

- [ ] Verify `pipeline_stage=full` creates raw, compute and output rings from the compiler ring plan,
  starts output dbnull before `pipeline_worker`, starts unpack before receiver, then starts senders.
- [ ] Verify readiness is stage-specific and a failure/early exit at any process aborts the complete
  transfer in reverse dependency order.
- [ ] Validate compute and output headers, final product bytes, `pipeline transfer completed`, dbnull
  EOD and all exit codes.
- [ ] Add deterministic known-data fixtures for Beamform-only, Beamform+Power,
  Beamform+Stokes and the legal integrated products.
- [ ] Add automated failure-path tests: one sender fails, worker rejects header, CUDA process fails,
  output consumer exits, timeout, partial EOD and cleanup.
- [ ] Run controller and campaign tests three consecutive times; no self-test may create remote rings.

## Task 6: Full-pipeline low-rate correctness acceptance

**Product changes:** none in the GPU server testing task.

- [ ] Build a fresh CUDA Release tree for SM86 with explicit PSRDADA paths.
- [ ] Run known-data `pipeline_stage=full` at a low rate three consecutive times.
- [ ] Compare final output header and samples with an independent CPU oracle for each enabled product
  chain selected for the first release.
- [ ] Reconcile sender→receiver→unpack→pipeline_worker→output bytes and clean EOD/cleanup.

**Completion:** the exact two-ring GPU application boundary passes three clean repetitions before any
full-pipeline rate is reported.

## Task 7: Full-pipeline rate ladder and headroom

**Versioned entry:** `scripts/atfp_throughput_campaign.py`

- [ ] Run preflight-only and retain the feasibility report from Task 4.
- [ ] Execute 1, 5, 10, 20, 30, 35 and 40 Gbps in order, with one warm-up and three measured runs
  per point; stop at the first failure.
- [ ] For each run record H2D, conversion, Beamform/product/integration, D2H and output wait/service
  times in addition to ingest/unpack evidence.
- [ ] If the first failure interval is broad, bisect only after the failing component is identified.
- [ ] Define the production target below the highest stable rate so measured stage utilization leaves
  the Task 4 headroom; do not operate at the single best observed rate.

**Completion:** report the highest repeatable full-pipeline payload rate, the limiting stage, and a
recommended operating rate with explicit headroom.

## Task 8: Decision gate for subsequent development

- [ ] Update `docs/PROJECT_STATUS.md` and `docs/VDIF_UNPACK_STATUS.md` with accepted artifacts only.
- [ ] Notify the user that tested changes are ready for a Git commit; do not commit automatically.
- [ ] After user-authorized commit/push, start module registry development.
- [ ] After registry acceptance, start `pipelinectl`; then continuous-observation/fault testing;
  develop `dada2rdma` last.
