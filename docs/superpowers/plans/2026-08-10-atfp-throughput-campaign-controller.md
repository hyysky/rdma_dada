# ATFP Throughput Campaign Controller Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build one versioned controller that runs the full UDP -> raw ring -> ATFP unpack -> GPU beamform -> output ring -> `dada_dbnull` path at ascending physical Ethernet line rates, bisects the first pass/fail interval to 0.5 Gbps, and preserves evidence identifying the first saturated stage.

**Architecture:** Keep `scripts/task8c_rate_point.py` as the single-rate remote execution primitive, extend that primitive to include `pipeline_worker` and the output ring, and add `scripts/atfp_throughput_campaign.py` as the only multi-rate acceptance entry. The campaign controller converts physical wire rate into the existing sender-record rate, runs immutable preflight/build identities once, executes one warm-up plus three measured 30-second runs per point, and owns adaptive search, locking, resume, summaries and bottleneck classification.

**Tech Stack:** Python 3 standard library, existing C++/CUDA pipeline applications, PSRDADA, SSH/SCP, JSON, unittest/CTest.

## Global Constraints

- Physical wire-rate points are exactly `1, 5, 10, 20, 30, 35, 40 Gbps`; 40 Gbps is an upper probe.
- Every warm-up and measured repetition lasts 30 seconds.
- Every rate point has one warm-up and three measured repetitions.
- After the first failed point, bisect the last-pass/first-fail interval until its width is at most 0.5 Gbps.
- The baseline GPU chain is H2D -> ATFP-to-[TFP,A] transpose/CI conversion/scale -> beamform -> D2H; Power, Stokes and integration are excluded.
- `dada_dbnull -k OUTPUT_KEY -s -z -q` consumes the final output-ring transfer.
- Both Stations are mandatory; one sender failure aborts both senders and the receiver-side pipeline.
- Formal server acceptance uses only the versioned campaign entry; direct commands are diagnostic gates only.
- Preserve unrelated dirty worktree files and do not commit or push without explicit user authorization.

---

### Task 1: Physical Wire-Rate Model

**Files:**
- Create: `scripts/atfp_throughput_campaign.py`
- Create: `tests/atfp_throughput_campaign_test.py`
- Create: `config/testing/atfp-throughput-campaign.json`

**Interfaces:**
- Produces: `WireModel`, `CampaignConfig`, `load_campaign_config(path)`, and `wire_gbps_to_record_gbps(target_wire_gbps, record_bytes, station_count)`.
- Consumes: Project VDIF `record_bytes` from the compiler-resolved plan.

- [ ] **Step 1: Write failing tests for the wire model and strict campaign JSON**

Assert that untagged IPv4 accounting contains Ethernet header 14, IPv4 20, UDP 8, FCS 4, preamble/SFD 8 and IFG 12 bytes; that a 4,128-byte VDIF record has 4,194 physical bytes; that aggregate wire rate is split equally across two Stations; and that unknown keys, duplicate/nonascending rates, duration other than 30, repetitions other than 1+3, non-dbnull consumer and a bisection tolerance other than 0.5 are rejected.

- [ ] **Step 2: Run the new test and verify RED**

Run: `python3 tests/atfp_throughput_campaign_test.py`

Expected: import/file failure because the campaign controller does not exist.

- [ ] **Step 3: Implement strict dataclasses and conversion**

Implement immutable `WireModel` and `CampaignConfig`; calculate:

```python
physical_bytes = record_bytes + 14 + 20 + 8 + 4 + 8 + 12
aggregate_record_gbps = target_wire_gbps * record_bytes / physical_bytes
per_station_record_gbps = aggregate_record_gbps / station_count
```

Store both the input wire rate and derived record rates in serialized plans. Reject VLAN/IPv6 fields rather than silently changing the model.

- [ ] **Step 4: Run the test and verify GREEN**

Run: `python3 tests/atfp_throughput_campaign_test.py`

Expected: all Task 1 tests pass.

### Task 2: Full Single-Rate GPU Pipeline Primitive

**Files:**
- Modify: `scripts/task8c_rate_point.py`
- Modify: `tests/task8c_rate_point_test.py`

**Interfaces:**
- Produces: `RatePlan.output_key`, `RatePlan.output_block_bytes`, `RatePlan.output_ring_blocks`; a qths bundle that starts three rings, unpack, CUDA `pipeline_worker`, and final-ring `dada_dbnull`; parsed receiver/unpack/GPU/consumer evidence.
- Consumes: compiler artifacts `resolved_observation.json`, `ring_plan.json`, `raw.header`, `unpacked.header`, `output.header`; exact qths Release binaries `rdma2dada`, `vdif_unpack_worker`, `pipeline_worker`.

- [ ] **Step 1: Write failing artifact and bundle tests**

Extend fixtures to include `output.header` and `rings.output`. Assert strict identity/block/key validation; assert `prepare.sh` creates the output ring with compiler-derived capacity; assert start order is output consumer, `pipeline_worker --plan`, unpack, receiver; assert finish/cleanup owns the output ring and worker PID; assert dbnull uses the output key with `-s -z -q`.

- [ ] **Step 2: Run focused tests and verify RED**

Run: `python3 -m unittest tests.task8c_rate_point_test`

Expected: failures for missing output artifact/properties and absent `pipeline_worker` orchestration.

- [ ] **Step 3: Extend `RatePlan` and qths bundle minimally**

Load and hash `output.header`; expose compiler-derived output key/block/ring count. Require an explicit validated qths binary directory containing all three applications. Create/destroy only the plan's raw, compute and output keys. Start `dada_dbnull` on output, then `pipeline_worker`, unpack and receiver; record exact PIDs and SHA256 values. Preserve the existing abort-on-one-Station behavior.

- [ ] **Step 4: Add GPU and output evidence parsing**

Parse versioned `pipeline_worker` completion/counter output and final dbnull exit. Keep receiver, unpack, GPU worker and output consumer as separate result sections. Require CONFIG_ID/GEOMETRY_ID agreement and exact input/output block accounting. When timing or ring occupancy fields are unavailable, record them as missing evidence and classify the bottleneck `UNDETERMINED`; never invent values.

- [ ] **Step 5: Run focused and regression tests**

Run three times:

```bash
python3 -m unittest tests.task8c_rate_point_test
```

Expected: three clean passes; existing sender failure, cleanup, partial raw block and dbdisk diagnostic tests remain green.

### Task 3: Versioned Fixed-Point and Bisection Campaign

**Files:**
- Modify: `scripts/atfp_throughput_campaign.py`
- Modify: `tests/atfp_throughput_campaign_test.py`

**Interfaces:**
- Produces: `CampaignController.run()`, `next_rate_point()`, campaign lock, per-rate summaries, resume validation and `bottleneck_report.json`.
- Consumes: Task 1 wire conversion and Task 2 `run_rate_request_sequence` single-rate primitive.

- [ ] **Step 1: Write failing scheduler tests**

Cover all-fixed-pass, first-point-fail, 30-pass/35-fail bisection, exact 0.5-Gbps termination, no higher fixed point after failure, one warm-up plus three measured calls, failure propagation, incomplete-point restart, identity-mismatch resume rejection and active-lock refusal.

- [ ] **Step 2: Run the campaign tests and verify RED**

Run: `python3 tests/atfp_throughput_campaign_test.py`

Expected: failures for absent scheduling and resume functions.

- [ ] **Step 3: Implement campaign state machine**

Use ascending fixed points. After first failure, calculate a Decimal midpoint between last PASS and first FAIL; quantize only for stable directory names, not search decisions. Stop when `fail_rate - pass_rate <= Decimal("0.5")`. If 1 Gbps fails, report no stable lower bound. If 40 Gbps passes, report no bottleneck reached in range.

- [ ] **Step 4: Implement immutable identity and scoped locking**

Write `campaign.json`, `environment.json`, `manifest.sha256` and atomic `state.json`. Refuse resume unless campaign config, source, configuration, binary/library and Phase 0 identities match. Restart an incomplete point at warm-up. Use an exclusive campaign lock containing campaign ID/PID/start time; do not remove a live foreign lock.

- [ ] **Step 5: Implement bottleneck classification**

Classify NIC only from run-local counter deltas, unpack from complete receiver input plus growing raw-ring occupancy/service deficit, and GPU from complete unpack output plus growing compute-ring occupancy/service deficit. Otherwise return `UNDETERMINED`. Keep `TEST_RESULT` and `CLEANUP_RESULT` independent.

- [ ] **Step 6: Run campaign tests three times**

Run three times:

```bash
python3 tests/atfp_throughput_campaign_test.py
```

Expected: every run passes with deterministic rate ordering and summaries.

### Task 4: Formal CLI, Documentation and Local Acceptance Gates

**Files:**
- Modify: `scripts/atfp_throughput_campaign.py`
- Modify: `CMakeLists.txt`
- Modify: `docs/agents/testing.md`
- Modify: `tests/README.md`

**Interfaces:**
- Produces: one documented `--preflight-only` and `--execute` campaign command using explicit observation config/compiler/qths/sender binary directories.
- Consumes: Tasks 1-3.

- [ ] **Step 1: Write failing CLI tests**

Assert `--execute` rejects missing explicit Release directories, duration/repetition overrides, dbdisk, stale/default `build-linux`, missing manifest identities and an unavailable output consumer. Assert `--preflight-only` exercises the same parsing, binary and endpoint gates without creating rings.

- [ ] **Step 2: Run tests and verify RED**

Run: `python3 tests/atfp_throughput_campaign_test.py`

Expected: CLI-policy tests fail until the final parser is implemented.

- [ ] **Step 3: Implement the formal CLI and CTest registration**

Expose only campaign config, result root, project root, known-hosts, observation config, compiler, qths Release directory, sender Release directory, source manifest, `--preflight-only`, `--execute` and `--resume`. Do not expose manual rate/duration/repetition overrides for formal execution. Register the two Python controller suites with CTest using the discovered Python interpreter.

- [ ] **Step 4: Document the single entry and resource ownership**

Document Phase 0, exact paired qths CMake/CTest paths, complete-tree source mirroring, SHA gate, output-ring dbnull semantics, fixed rates, 30-second 1+3 repetitions, bisection, lock/queue behavior, result tree, resume boundary and failure classes.

- [ ] **Step 5: Run local acceptance**

Run:

```bash
python3 tests/task8c_rate_point_test.py
python3 tests/atfp_throughput_campaign_test.py
python3 -m py_compile scripts/task8c_rate_point.py scripts/atfp_throughput_campaign.py
cmake -S . -B build-controller-local -DBUILD_TESTING=ON -DBUILD_RDMA_PIPELINE=OFF -DUSE_CUDA=OFF
ctest --test-dir build-controller-local -R '^(task8c_rate_point_test|atfp_throughput_campaign_test)$' --output-on-failure
```

Expected: all commands pass. Inspect dry-run output to confirm the physical-wire conversion and ordered points, but do not treat dry-run as remote acceptance.

### Task 5: GPU-Server Handoff and Formal Campaign

**Files:**
- No product-code changes in the testing task.

**Interfaces:**
- Produces: callback report with Phase 0, synchronization SHA gate, fresh Release identities, controller gates, preflight, every repetition, adaptive-search result, bottleneck evidence and scoped cleanup.
- Consumes: reviewed local controller worktree and its supplied SHA256 manifest.

- [ ] **Step 1: Notify `GPU服务器代码测试` with exact scope**

Request complete-tree mirroring from the local worktree while excluding `.git`, build/result/data; exact SHA verification; read-only Phase 0 on HF/qths1/qtpulsar1/qtpulsar2; fresh Release builds; paired qths CMake/CTest 3.31.12 gates; and a formal preflight using explicit binary directories.

- [ ] **Step 2: Require preflight callback before formal execution**

Verify exact PSRDADA paths/options, `dada_dbnull -s -z`, NIC/link/NUMA/GPU state, output ring geometry, CUDA worker, sender endpoints, source manifest and no foreign task-owned resource collision. Any mismatch returns `ENV_BLOCKED`, `SYNC_FAIL` or `HARNESS_FAIL` before rings are created.

- [ ] **Step 3: Run the versioned formal campaign**

After preflight PASS, execute the single documented campaign command. Do not issue hand-written rate-point commands. Wait for the result callback and preserve the task cursor until completion.

- [ ] **Step 4: Evaluate and close the development-test loop**

If a harness/product/performance failure occurs, write a failing local regression test before changing code, repeat local gates, then hand off again. When the remote task reports PASS or a valid saturation boundary with cleanup PASS, report the maximum stable physical wire-rate interval and evidence-backed NIC/unpack/GPU bottleneck to the user. Remind the user that tested changes are ready for their commit decision; do not commit or push automatically.
