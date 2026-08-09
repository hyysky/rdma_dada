# Unified Observation Configuration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace independently maintained pipeline, packet-geometry, unpack and worker configuration with one user-authored observation JSON and one compiler-produced resolved plan shared by every process.

**Architecture:** A portable C++ configuration layer strictly parses the observation JSON and fixed Project VDIF profile, derives all geometry with checked integer arithmetic, and serializes a canonical `ResolvedObservationPlan`. A compiler CLI emits the resolved plan, DADA headers, ring plan, validation report and SHA256 manifest. `rdma2dada`, `vdif_unpack_worker`, `pipeline_worker` and the versioned test controller consume the same resolved plan and reject mixed identities before creating or using resources.

**Tech Stack:** C++11, existing strict JSON parser, portable in-tree SHA256, CMake, PSRDADA ASCII headers, Python 3 test controller, CUDA 12.8 integration after the existing ATFP conversion tasks.

## Global Constraints

- The user authors one observation JSON; no user-authored byte size is accepted.
- Project VDIF remains a fixed 32-byte header with TFP payload, CI8, IQ, little-endian and `TWOS_COMPLEMENT` in the first schema.
- `station_ids` order defines the A axis; `NANT=station_ids.size()` and is never independently configured.
- `duration_seconds` is an exact decimal string and must resolve to an integer number of groups.
- Raw ring data retains the 32-byte VDIF header; ATFP window and compute ring data contain payload only.
- `groups_per_block` is the sole block-geometry policy input; all raw, window, compute, ring, file and transfer byte counts are derived.
- The fixed wire profile contains protocol structure only and contains no observation-specific payload byte count or axis extent.
- Every process validates the same `CONFIG_ID` and `GEOMETRY_ID` before processing.
- Preflight must finish before ring creation, capability changes or process startup.
- Existing JSON examples are retained until migration is accepted and the user separately approves deletion.
- Portable local tests use `BUILD_RDMA_PIPELINE=OFF` and `USE_CUDA=OFF`; server builds use Release.
- No Git command is run on test hosts. Synchronization uses a development-supplied SHA256 manifest.
- Do not commit or push automatically. After a server PASS, report that the checkpoint is ready and let the user decide.
- Preserve unrelated dirty-worktree changes, especially TimeIntegrate benchmark and optimization files.

## Execution Order

Tasks 1–7 form the immediate configuration migration and unblock the unfinished ATFP Task 5 finite-transfer test. Task 8 is completed after Tasks 6–8 in `docs/superpowers/plans/2026-08-08-atfp-unpack-gpu-transpose.md`, because the production ATFP-to-TFPA header transform must exist before the compiler can validate the complete GPU algorithm chain without duplicating module logic.

## Test and Handoff Protocol

For every task:

1. Add the failing test before product code.
2. Run the focused test and confirm the intended failure.
3. Implement the smallest behavior that satisfies the test.
4. Run focused and adjacent tests locally.
5. Produce SHA256 values for affected files.
6. Notify `GPU服务器代码测试` with exact files, build commands, fixtures and acceptance criteria.
7. Wait for the callback. On failure, diagnose, fix locally and repeat the same test before advancing.

---

### Task 1: Strict Observation JSON Parser

**Files:**
- Create: `include/rdma_dada/config/observation_config.h`
- Create: `src/config/observation_config.cpp`
- Create: `config/observation-v1.schema.json`
- Create: `config/observation.example.json`
- Create: `tests/observation_config_test.cpp`
- Modify: `CMakeLists.txt`
- Modify: `config/README.md`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: one user-authored observation JSON and paths resolved relative to that JSON.
- Produces:

```cpp
namespace rdma_dada {

enum class ObservationModuleKind {
    kBeamform,
    kPower,
    kStokes,
    kIntegrate
};

struct ObservationModuleConfig {
    ObservationModuleKind kind;
    std::string weights_file;
    std::string weights_order;
    std::string weights_id;
    std::string weights_scale;
    std::string compute_mode;
    std::uint64_t integration_length;
    std::string integration_operation;
};

struct ObservationConfig {
    std::uint32_t schema_version;
    std::string source_path;
    std::string observation_id;
    std::string utc_start;
    std::string duration_seconds;
    std::uint64_t duration_ps;
    std::vector<std::uint16_t> station_ids;
    std::uint16_t first_channel_id;
    std::uint32_t nchan;
    std::uint32_t npol;
    std::uint64_t sample_interval_ps;
    std::string telescope;
    std::uint64_t bandwidth_hz;
    std::uint64_t center_frequency_hz;
    std::string wire_profile_path;
    std::uint64_t samples_per_packet;
    std::uint64_t groups_per_block;
    std::uint64_t raw_ring_blocks;
    std::uint64_t compute_ring_blocks;
    std::uint64_t window_blocks;
    std::uint32_t raw_key;
    std::uint32_t compute_key;
    bool disk_enabled;
    std::uint64_t blocks_per_file;
    bool direct_io;
    std::string receiver_device;
    std::string destination_mac;
    std::string destination_ip;
    std::uint16_t destination_port;
    std::string backend;
    int cuda_device;
    bool run_once;
    std::vector<ObservationModuleConfig> modules;
};

bool ParseExactSecondsToPicoseconds(const std::string& text,
                                    std::uint64_t* picoseconds,
                                    std::string* error);
bool LoadObservationConfig(const std::string& path,
                           ObservationConfig* config,
                           std::string* error);
}
```

- [x] **Step 1: Add failing exact-duration tests**

  Test `"10"`, `"0.000000000001"`, `"7.752192"`, zero, negative, exponent notation, more than 12 fractional digits and uint64 overflow.

  Representative assertion:

  ```cpp
  std::uint64_t value = 0;
  Expect(ParseExactSecondsToPicoseconds("7.752192", &value, &error),
         "exact duration parses");
  Expect(value == UINT64_C(7752192000000), "exact duration value");
  ```

- [x] **Step 2: Add failing strict-schema tests**

  Load `config/observation.example.json`; verify `telescope=CA`,
  `bandwidth_hz=300000000`, `center_frequency_hz=1250000000`, relative
  profile/weight paths, Station ordering, ring keys and processing fields.
  Allow an empty module list for raw/unpack-only operation. Reject missing
  fields, unknown fields, empty telescope, zero frequency metadata, duplicate
  Station IDs, empty Station list, `NPOL` outside 1/2, zero block policy,
  invalid IP/MAC/key syntax and invalid module order.

- [x] **Step 3: Run the tests and confirm failure**

  ```bash
  cmake -S . -B build-observation-local \
    -DBUILD_TESTING=ON -DBUILD_RDMA_PIPELINE=OFF -DUSE_CUDA=OFF \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build build-observation-local --target observation_config_test
  ctest --test-dir build-observation-local \
    -R '^observation_config_test$' --output-on-failure
  ```

  Expected before implementation: compile failure because the observation API is absent.

- [x] **Step 4: Implement exact parsing and strict JSON ownership**

  Reuse `json::Value`, require exact keys at every object, reject duplicated Station IDs and resolve paths relative to `source_path`. Keep JSON numbers for integer fields and decimal strings for `duration_seconds` and `weights_scale`.

- [x] **Step 5: Run focused tests three times**

  ```bash
  ctest --test-dir build-observation-local -R '^observation_config_test$' --repeat until-fail:3 --output-on-failure
  ```

- [x] **Step 6: Hand off Task 1 to the GPU-server test task**

  Require a Linux Release portable build and three direct repetitions. No PSRDADA ring, sender, receiver, CUDA context or capability may be created.

---

### Task 2: Remove Observation Geometry from the Wire Profile

**Files:**
- Modify: `include/rdma_dada/config/packet_format_config.h`
- Modify: `src/config/packet_format_config.cpp`
- Create: `config/packet_formats/packet-format-v2.schema.json`
- Modify: `config/packet_formats/frontend.example-v1.json`
- Modify: `config/packet_formats/README.md`
- Modify: `tests/packet_format_config_test.cpp`
- Modify: `tests/packet_format_inspect_test.py`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: developer-owned Project VDIF wire profile schema version 2.
- Produces: `PacketFormatConfig` without observation-specific `payload_bytes`.

```cpp
struct PacketFormatConfig {
    std::uint32_t schema_version;
    std::string format_id;
    std::uint64_t application_header_bytes;
    std::string bit_numbering;
    std::vector<ApplicationHeaderField> header_fields;
    std::string sample_format;
    std::string sample_encoding;
    std::string component_order;
    PacketEndianness payload_endianness;
    std::vector<std::string> packed_order;
    std::vector<PacketPayloadAxis> axes;
};
```

- [x] **Step 1: Add failing profile-separation tests**

  Require schema version 2, `application_header_bytes=32`, CI8, two's-complement, IQ, little-endian and `packed_order=[T,F,P]`. Reject `record.payload_bytes`, literal observation extents and unknown `output_order`/signed fields.

- [x] **Step 2: Run profile tests and confirm failure**

  ```bash
  cmake --build build-observation-local --target packet_format_config_test packet_format_inspect
  ctest --test-dir build-observation-local \
    -R '^(packet_format_config_test|packet_format_inspect_test)$' --output-on-failure
  ```

- [x] **Step 3: Update the profile model and schema**

  Remove `PacketFormatConfig::payload_bytes`. Change `record` to contain only `application_header_bytes`. Keep header-derived T/F/P axes and Station lookup A-axis. Make schema-version-1 profiles fail with an explicit migration error in the new observation compiler.

- [x] **Step 4: Run packet and VDIF parser regressions three times**

  ```bash
  cmake --build build-observation-local --target \
    packet_format_config_test packet_format_inspect project_vdif_v1_test
  ctest --test-dir build-observation-local \
    -R '^(packet_format_config_test|packet_format_inspect_test|project_vdif_v1_test)$' \
    --repeat until-fail:3 --output-on-failure
  ```

- [x] **Step 5: Hand off Task 2 and wait for callback**

  Acceptance requires the synchronized profile and schema hashes, strict rejection of duplicated geometry and three Release repetitions.

---

### Task 3: Resolve Geometry, Timeline and Ring Sizes Once

**Files:**
- Create: `include/rdma_dada/config/resolved_observation_plan.h`
- Create: `src/config/resolved_observation_plan.cpp`
- Create: `tests/resolved_observation_plan_test.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: `ObservationConfig` and `PacketFormatConfig`.
- Produces:

```cpp
struct ResolvedObservationPlan {
    ObservationConfig source;
    PacketFormatConfig wire;
    std::uint32_t nant;
    std::uint64_t complex_sample_bytes;
    std::uint64_t payload_bytes;
    std::uint64_t raw_record_bytes;
    std::uint64_t group_period_ps;
    std::uint64_t expected_groups;
    std::uint64_t records_per_block;
    std::uint64_t samples_per_block;
    std::uint64_t raw_block_bytes;
    std::uint64_t compute_block_bytes;
    std::uint64_t window_groups;
    std::uint64_t window_payload_bytes;
    std::uint64_t window_validity_bytes;
    std::uint64_t raw_ring_bytes;
    std::uint64_t compute_ring_bytes;
    std::uint64_t raw_file_bytes;
    std::uint64_t compute_file_bytes;
    std::uint64_t payload_bytes_per_second;
    std::uint64_t raw_bytes_per_second;
    std::uint8_t group_start_reference_epoch;
    std::uint32_t group_start_seconds;
    std::uint32_t group_start_frame;
    std::string config_id;
    std::string geometry_id;
};

bool ResolveObservationPlan(const ObservationConfig& config,
                            const PacketFormatConfig& wire,
                            ResolvedObservationPlan* plan,
                            std::string* error);
```

- [x] **Step 1: Add failing exact-geometry tests**

  Use `A=2,F=2,P=2,T_pkt=512,CI8,G=1024,window_blocks=2,duration=7.752192s`. Require:

  ```text
  payload_bytes       = 4096
  raw_record_bytes    = 4128
  group_period_ps     = 512000000
  expected_groups     = 15141
  records_per_block   = 2048
  samples_per_block   = 524288
  raw_block_bytes     = 8454144
  compute_block_bytes = 8388608
  window_groups       = 2048
  window_payload      = 16777216
  payload_Bps         = 16000000
  raw_Bps             = 16125000
  ```

- [x] **Step 2: Add failing invariant and overflow tests**

  Cover duration remainder, payload/header multiplication overflow, VDIF frame-length overflow, `NCHAN>255`, direct-I/O misalignment, duplicate Station IDs, invalid UTC and a non-32-byte profile.

- [x] **Step 3: Run the resolver test and confirm failure**

  ```bash
  cmake --build build-observation-local --target resolved_observation_plan_test
  ctest --test-dir build-observation-local -R '^resolved_observation_plan_test$' --output-on-failure
  ```

- [x] **Step 4: Implement checked integer derivation**

  Centralize checked add, multiply, division and round-up helpers in `resolved_observation_plan.cpp`. Derive `GROUP_START_REFERENCE_EPOCH`, `GROUP_START_SECONDS` and `GROUP_START_FRAME=0` from integer-second `UTC_START` using the same VDIF epoch rules as packet parsing.

- [x] **Step 5: Run config/profile/resolver tests three times**

  ```bash
  ctest --test-dir build-observation-local \
    -R '^(observation_config_test|packet_format_config_test|resolved_observation_plan_test)$' \
    --repeat until-fail:3 --output-on-failure
  ```

- [x] **Step 6: Hand off Task 3 and wait for callback**

  Acceptance requires exact values above on qths1 Release and no resource creation.

---

### Task 4: Canonical Serialization and Portable SHA256 Identity

**Files:**
- Create: `include/rdma_dada/config/sha256.h`
- Create: `src/config/sha256.cpp`
- Create: `include/rdma_dada/config/resolved_plan_json.h`
- Create: `src/config/resolved_plan_json.cpp`
- Create: `tests/config_identity_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: resolved plan, exact wire-profile bytes and weight-file bytes.
- Produces:

```cpp
std::string Sha256Hex(const void* data, std::size_t size);
bool SerializeResolvedObservationPlan(const ResolvedObservationPlan& plan,
                                      std::string* json,
                                      std::string* error);
bool LoadResolvedObservationPlan(const std::string& path,
                                 ResolvedObservationPlan* plan,
                                 std::string* error);
bool ComputeObservationIdentities(ResolvedObservationPlan* plan,
                                  std::string* error);
```

- [x] **Step 1: Add failing hash and canonicalization tests**

  Require the standard SHA256 value for `"abc"`:

  ```text
  ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
  ```

  Require identical IDs for observation JSON files differing only in whitespace/key order, changed `CONFIG_ID` for receiver or weight contents, and changed `GEOMETRY_ID` only for geometry changes.

- [x] **Step 2: Run the identity test and confirm failure**

  ```bash
  cmake --build build-observation-local --target config_identity_test
  ctest --test-dir build-observation-local -R '^config_identity_test$' --output-on-failure
  ```

- [x] **Step 3: Implement dependency-free SHA256 and canonical JSON**

  Keep SHA256 inside `rdma_pipeline_config`; do not add OpenSSL or platform-specific commands. Serialize keys in fixed lexical order, integers in decimal, booleans as JSON literals and exact decimal strings unchanged. Hash referenced file contents, not only paths.

- [x] **Step 4: Reject identity tampering on reload**

  `LoadResolvedObservationPlan` recomputes both IDs and rejects a stored value that differs. The identity calculation excludes the two identity fields themselves.

- [x] **Step 5: Run identity and resolver tests three times, then hand off**

  Server acceptance compares the C++ SHA result with `/usr/bin/sha256sum` as evidence but does not make the application depend on that executable.

---

### Task 5: Compile DADA Headers and Artifact Bundle

**Files:**
- Create: `include/rdma_dada/config/observation_artifacts.h`
- Create: `src/config/observation_artifacts.cpp`
- Create: `tools/observation_config_compile.cpp`
- Create: `tests/observation_artifacts_test.cpp`
- Create: `tests/observation_config_compile_test.py`
- Modify: `src/pipeline/dada_header_builder.cpp`
- Modify: `include/rdma_dada/pipeline/dada_header.h`
- Modify: `src/io/psrdada/header_codec.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: one observation JSON.
- Produces an atomic output directory containing:

```text
resolved_observation.json
ring_plan.json
raw.header
unpacked.header
validation_report.json
MANIFEST.sha256
```

```cpp
struct ObservationArtifacts {
    ResolvedObservationPlan plan;
    pipeline::Metadata raw_header;
    pipeline::Metadata unpacked_header;
    std::string validation_report_json;
};

bool BuildObservationArtifacts(const std::string& observation_path,
                               ObservationArtifacts* artifacts,
                               std::string* error);
bool WriteObservationArtifacts(const ObservationArtifacts& artifacts,
                               const std::string& output_directory,
                               std::string* error);
```

`src/config/observation_artifacts.cpp` belongs to a new
`rdma_observation_compiler` target declared after `rdma_module_vdif_unpack` and
linked against that module plus `rdma_pipeline_config`. This avoids introducing
a reverse dependency from the portable configuration library to the unpack
module.

- [x] **Step 1: Add failing byte-exact header tests**

  Require RAW fields including `CONFIG_ID`, `GEOMETRY_ID`, `TELESCOPE=CA`,
  `BANDWIDTH_HZ=300000000`, `CENTER_FREQUENCY_HZ=1250000000`,
  `GROUP_PERIOD_PS`, start epoch/seconds/frame, `EXPECTED_GROUPS`,
  `RECORD_BYTES`, `RESOLUTION`, byte rates and file size. Feed that Metadata
  through `BuildVdifUnpackOutputHeader` and require ATFP fields, metadata
  preservation, header removal and transfer size.

- [x] **Step 2: Add failing CLI tests**

  Required commands:

  ```bash
  observation_config_compile --config config/observation.example.json --preflight-only
  observation_config_compile --config config/observation.example.json --output-dir /tmp/observation-artifacts
  ```

  `--preflight-only` prints the validation report, writes nothing and exits nonzero on invalid input. Output mode writes into a sibling staging directory, fsyncs files, renames once and refuses to overwrite an existing directory.

- [x] **Step 3: Implement raw and unpacked header compilation**

  Extend the runtime header model with identity and timeline fields. Stop relying on unknown fields preserved from a static template. Reuse `BuildVdifUnpackOutputHeader`; do not reproduce its ATFP calculations in the compiler.

- [x] **Step 4: Implement reports and manifest**

  `validation_report.json` records inputs, derived values, formulas, supported stage headers and each check result. `MANIFEST.sha256` hashes every generated file except itself, using two spaces between digest and relative filename.

- [x] **Step 5: Run focused tests and CLI execution-path tests three times**

  ```bash
  cmake --build build-observation-local --target \
    observation_artifacts_test observation_config_compile
  ctest --test-dir build-observation-local \
    -R '^(observation_artifacts_test|observation_config_compile_test)$' \
    --repeat until-fail:3 --output-on-failure
  ```

- [x] **Step 6: Hand off Task 5 and wait for callback**

  Require qths1 Release tests, three clean compiler runs, manifest verification, and proof that preflight created no process, IPC segment or capability.

---

### Task 6: Make RDMA Receiver and VDIF Unpack Consume the Resolved Plan

**Files:**
- Modify: `apps/rdma2dada/main.cpp`
- Modify: `apps/rdma2dada/README.md`
- Modify: `apps/vdif_unpack_worker/main.cpp`
- Modify: `modules/vdif_unpack/vdif_unpack_config.cpp`
- Modify: `include/rdma_dada/modules/vdif_unpack/vdif_unpack_config.h`
- Modify: `apps/vdif_unpack_worker/README.md`
- Modify: `tests/vdif_unpack_config_test.cpp`
- Modify: `tests/vdif_unpack_header_test.cpp`
- Modify: `tests/vdif_unpack_worker_integration.sh`
- Modify: `tests/dada_header_roundtrip_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `resolved_observation.json` generated by Task 5.
- Produces: identical runtime geometry and header identity in both processes.

- [x] **Step 1: Add failing process-config tests**

  Require `rdma2dada --plan <resolved>` to use receiver endpoint, raw key and exact raw geometry from the plan. Require `vdif_unpack_worker --plan <resolved>` to use input/output keys, Station map, profile, window geometry and run-once policy from the same plan.

  Reject:

  ```text
  modified CONFIG_ID
  modified GEOMETRY_ID
  raw header field mismatch
  actual raw ring block capacity mismatch
  actual compute ring block capacity mismatch
  geometry-changing command-line overrides
  ```

- [x] **Step 2: Run focused tests and confirm failure**

  ```bash
  cmake --build build-observation-local --target \
    vdif_unpack_config_test vdif_unpack_header_test
  ctest --test-dir build-observation-local \
    -R '^(vdif_unpack_config_test|vdif_unpack_header_test)$' --output-on-failure
  ```

- [x] **Step 3: Replace independent config loading**

  Remove the unpack worker's `sources.pipeline_config` and `sources.packet_format` runtime dependency. Construct its `VdifUnpackConfig`, `PipelineConfig` adapter and layout from the resolved plan in memory. Make `rdma2dada` build and publish `raw.header` directly; `--dump-header` remains diagnostic output only and is never an input template.

- [x] **Step 4: Validate IDs and capacities before data processing**

  In worker open callbacks, compare the DADA header IDs and all required geometry against the resolved plan before configuring the engine. Fail the transfer without publishing data when any field differs.

- [x] **Step 5: Run portable tests, then Linux PSRDADA integration three times**

  Portable:

  ```bash
  ctest --test-dir build-observation-local \
    -R '^(observation_config_test|resolved_observation_plan_test|vdif_unpack_config_test|vdif_unpack_header_test)$' \
    --repeat until-fail:3 --output-on-failure
  ```

  Server acceptance runs `vdif_unpack_worker_integration.sh` three clean repetitions with generated artifacts and verifies no static header template is read.

- [x] **Step 6: Hand off Task 6 and wait for callback**

  Require Release, absolute PSRDADA paths from Phase 0, exact header/plan IDs, EOD, and clean resource teardown.

---

### Task 7: Migrate the Versioned Multi-Host Test Controller

**Files:**
- Modify: `scripts/task8c_rate_point.py`
- Modify: `tests/task8c_rate_point_test.py`
- Modify: `docs/agents/testing.md`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: one per-run observation JSON plus test-only sender endpoints/rate/fault settings.
- Produces: compiler-generated runtime artifacts and reproducible acceptance evidence.

- [x] **Step 1: Add failing controller regression tests**

  Require the controller to:

  ```text
  run Phase 0 before generating or changing the harness
  generate one observation JSON per run
  invoke observation_config_compile --preflight-only
  invoke output mode and verify MANIFEST.sha256
  transfer the verified bundle to qths1
  use no static .header template
  use no legacy pipeline.json/packet.json/worker.json geometry template
  stop before ring creation on any preflight or manifest failure
  abort the observation when either Station sender fails
  keep test result, diagnostic collection and cleanup status independent
  ```

- [x] **Step 2: Run controller tests and confirm failure**

  ```bash
  python3 tests/task8c_rate_point_test.py
  ```

- [x] **Step 3: Replace duplicated Python geometry**

  Keep rate pacing and test-only source endpoints in Python. Read all packet counts, block bytes, timeline and expected counters from `validation_report.json` or `resolved_observation.json`. Do not reimplement C++ formulas in the controller.

- [x] **Step 4: Add `--preflight-only` to the authoritative runner**

  The controller's preflight checks Phase 0, compiler execution, bundle manifest, binary paths and endpoint availability, then exits before setcap/rings/processes. Formal `--execute` reruns the same gate immediately before resource creation.

- [x] **Step 5: Run automated controller paths three times**

  Cover success, invalid observation, stale profile, modified manifest, unavailable source port, sender early exit, receiver early exit, missing diagnostics and cleanup failure. Each test asserts the first failed stage and owned-resource cleanup.

- [x] **Step 6: Hand off the formal low-rate Task 5 acceptance**

  From HF, run three clean `0.1 Gbps` dbdisk repetitions using the versioned runner and generated configuration. Require exact sender, receiver, raw, unpack and compute accounting, partial raw EOD publication, ATFP bytes and cleanup PASS.

  Accepted on the GPU server in suite `20260809T011953Z-38a07dd9`:
  three measured repetitions passed with exact packet/group/byte accounting,
  sender-derived payload-prefix validation, ATFP output, EOD, and scoped cleanup.

- [x] **Step 7: Stop for the user's Git decision after PASS**

  Report the tested file scope and remind the user that the checkpoint is ready for a commit. Do not commit or push.

---

### Task 8: Integrate the Same Plan with the GPU Pipeline Worker

**Dependency:** Complete and accept Tasks 6–8 in `docs/superpowers/plans/2026-08-08-atfp-unpack-gpu-transpose.md` first.

**Files:**
- Modify: `include/rdma_dada/pipeline/worker_config.h`
- Modify: `src/pipeline/worker_config.cpp`
- Modify: `src/pipeline/module_chain.cpp`
- Modify: `apps/pipeline_worker/main.cpp`
- Modify: `apps/pipeline_worker/README.md`
- Modify: `src/config/observation_artifacts.cpp`
- Modify: `tests/pipeline_worker_core_test.cpp`
- Modify: `tests/pipeline_worker_cuda_chain_test.cpp`
- Modify: `tests/observation_artifacts_test.cpp`
- Modify: `tests/README.md`

**Interfaces:**
- Consumes: the same `resolved_observation.json` and input DADA header produced by unpack.
- Produces: complete expected header chain and final output geometry for the selected linear module chain.

- [ ] **Step 1: Add failing resolved-plan worker tests**

  Cover beamform-only, Power, Stokes with `NPOL=2`, Power plus integration and Stokes plus integration. Reject Stokes with `NPOL=1`, Power followed by Stokes, integration before product, mismatched weight digest/F/P/A/B shape, and input `CONFIG_ID`/`GEOMETRY_ID` mismatch.

- [ ] **Step 2: Replace independent worker geometry**

  Construct `WorkerConfig` from `ResolvedObservationPlan`; remove independently entered `input_geometry`. Verify the weight file digest before loading it. Preserve runtime-only backend/device fields from the resolved plan.

- [ ] **Step 3: Extend artifact compilation through production module transforms**

  Feed `UNPACKED/ATFP` through the accepted production conversion header transform, then `ModuleChain::Configure`. Record `converted.header`, `beamformed.header` and the configured final header in the artifact bundle and manifest.

- [ ] **Step 4: Validate actual ring capacity and output header before processing**

  `pipeline_worker` compares input header identities and block capacity with the plan, then compares its computed output capacity and header with the compiler's expected stage. Any mismatch fails before acquiring a data block.

- [ ] **Step 5: Run CPU configuration tests and GPU numerical chains**

  Run portable worker/config tests three times. Hand off RTX 3090 CUDA 12.8 Release tests for all five chains above, each three repetitions with exact or tolerance-based numerical checks and no stale artifact acceptance.

- [ ] **Step 6: Update the formal throughput runner and hand off**

  Add the GPU worker to the versioned controller using the same resolved plan. Use `dada_dbnull -s -z` after the final ring for throughput and dbdisk only for byte inspection. Record per-stage service time, ring occupancy and GPU/H2D/D2H timing.

- [ ] **Step 7: Report PASS and ask before legacy JSON deletion**

  After server acceptance, list the legacy example JSON files that are no longer consumed and ask the user which ones to delete. Do not delete, commit or push without the user's decision.

## Completion Criteria

- One user observation JSON is sufficient to derive every runtime geometry value.
- The fixed packet profile contains no observation-specific geometry.
- All processes consume the same resolved plan and verify identical IDs.
- Static DADA header templates and independently generated geometry are absent from formal runs.
- The compiler produces deterministic headers, report and manifest.
- Low-rate finite transfer passes three clean repetitions before rate testing resumes.
- After the accepted ATFP GPU conversion exists, the complete algorithm header chain is compiled and verified with the same plan.
