# Multi-Station FPGA Sender Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make two fixed-endpoint FPGA sender simulator processes emit every Station in a large Observation at the exact aggregate payload rate.

**Architecture:** Add a backward-compatible schema-v3 multi-Station sender contract, flatten time-group/Station coordinates only at the batching boundary, and let Task 8C partition the compiler-resolved Station list between the two physical sender hosts. Production receiver, unpack and GPU modules do not change.

**Tech Stack:** C++11, UDP/`sendmmsg`, Project VDIF v1, Python 3 controller and `unittest`, CMake/CTest.

**Spec:** `docs/superpowers/plans/2026-08-28-multi-station-sender-design.md`

## Global Constraints

- One fixed source IP/port and one socket per physical sender host.
- Preserve sender schemas 1 and 2.
- `group_count` counts time groups; record count is groups times local Stations.
- Station IDs come from Observation JSON; the sender never invents them.
- Preserve the 1 MHz rate represented by every channel.
- Do not modify `rdma2dada`, VDIF unpack, `pipeline_worker` or CUDA modules.
- Do not create a Git commit or push without explicit user authorization.

---

### Task 1: Schema-v3 Station list and record generation

**Files:**
- Modify: `include/rdma_dada/simulation/vdif_sender_sim.h`
- Modify: `src/simulation/vdif_sender_sim.cpp`
- Modify: `tests/vdif_sender_sim_test.cpp`

**Interfaces:**
- Produces: `VdifSenderSimConfig::station_ids` and Station-explicit header/record builders.

- [ ] Add failing tests loading schema v3, rejecting empty/duplicate IDs, and building the same group for distinct Station IDs.
- [ ] Run `vdif_sender_sim_test` and verify the new assertions fail for the missing schema.
- [ ] Implement schema-v3 parsing and Station-explicit builders while retaining schemas 1/2.
- [ ] Re-run `vdif_sender_sim_test` and verify it passes.

### Task 2: Flattened, rotated multi-Station batches

**Files:**
- Modify: `include/rdma_dada/simulation/vdif_sender_batch.h`
- Modify: `src/simulation/vdif_sender_batch.cpp`
- Modify: `tests/vdif_sender_batch_test.cpp`

**Interfaces:**
- Consumes: `VdifSenderSimConfig::station_ids`.
- Produces: packet views carrying time-group and Station identity.

- [ ] Add a failing literal-order test for Stations `[100,101,102]`: group 0 emits `100,101,102`; group 1 emits `101,102,100`.
- [ ] Run `vdif_sender_batch_test` and verify the new assertions fail.
- [ ] Make `Prepare` consume a flattened packet offset and derive group/rotated Station coordinates.
- [ ] Re-run `vdif_sender_batch_test` and verify it passes.

### Task 3: Runtime accounting and machine-readable summary

**Files:**
- Modify: `include/rdma_dada/simulation/udp_vdif_sender.h`
- Modify: `src/simulation/udp_vdif_sender.cpp`
- Modify: `apps/fpga_sender_sim/main.cpp`
- Modify: `tests/udp_vdif_sender_test.cpp`
- Modify: `tests/fpga_sender_sim_loopback_test.py`
- Modify: `tests/fpga_sender_sim_linux_batch_test.py`

**Interfaces:**
- Produces: aggregate and per-Station scheduled/sent JSON accounting.

- [ ] Add failing JSON and loopback assertions for one endpoint carrying multiple Stations.
- [ ] Run the focused tests and verify failures identify missing multi-Station accounting.
- [ ] Pace the flattened packet count, accumulate per-Station counts and emit schema-v3 JSON.
- [ ] Re-run focused C++ and loopback tests and verify they pass.

### Task 4: Controller partition and validation

**Files:**
- Modify: `scripts/task8c_rate_point.py`
- Modify: `tests/task8c_rate_point_test.py`

**Interfaces:**
- Produces: two `SenderSpec` objects with exact Station lists and proportional target rates.

- [ ] Add failing tests for deterministic 235/234 partitioning, fixed endpoints, 15.1575/15.093 Gbit/s VDIF-record rates and two generated configs.
- [ ] Run `tests/task8c_rate_point_test.py` and verify failures identify the two-Station restriction.
- [ ] Replace per-Station sender specs with per-host Station partitions and validate per-Station summary closure.
- [ ] Re-run the controller tests and verify all pass.

### Task 5: Documentation and complete local regression

**Files:**
- Modify: `apps/fpga_sender_sim/README.md`
- Modify: `docs/agents/testing.md`
- Modify: `tests/README.md`
- Modify: `config/testing/atfp-throughput-source-manifest.sha256`

**Interfaces:**
- Consumes: completed sender/controller behavior.
- Produces: reproducible local and server test entry documentation.

- [ ] Document schema v3, fixed endpoint semantics, Station partition and authoritative result fields.
- [ ] Configure a clean local build and run sender/controller focused tests plus their registered CTest entries.
- [ ] Regenerate and verify the complete source manifest.
- [ ] Notify `GPU服务器代码测试` with affected files, build commands, small functional acceptance, 30.016 Gbit/s signal-payload/30.2505 Gbit/s VDIF-record staged acceptance criteria.
