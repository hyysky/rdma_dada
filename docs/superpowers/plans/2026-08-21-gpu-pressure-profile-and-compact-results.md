# GPU Pressure, Baseline Profile, and Compact Results Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the four supported test boundaries reproducible by adding a rate-controlled `dada_junkdb -> GPU worker` test, enforcing the latest passing runtime profile, and retaining only complete machine-readable JSON plus one immutable evidence log per repetition.

**Architecture:** `scripts/task8c_rate_point.py` remains the public controller entry point, while profile resolution and artifact compaction move into focused support modules. A formal run resolves Observation JSON and a versioned host/topology profile before creating resources. GPU-only mode creates only compute/output rings, feeds block-aligned ATFP data with `dada_junkdb`, runs `pipeline_worker`, drains the output ring, validates the complete process ledger, and only then compacts verbose artifacts.

**Tech Stack:** Python 3.8+, JSON, PSRDADA (`dada_db`, `dada_junkdb`, `dada_dbnull`), CUDA `pipeline_worker`, CMake/CTest, `unittest`.

**Spec:** `docs/agents/testing.md`

## Global Constraints

- Supported formal topologies are exactly: `receive`, `unpack`, `gpu`, and `full`.
- `unpack` includes `rdma2dada + vdif_unpack_worker`; it is not a synthetic unpack-only producer.
- `gpu` means `dada_junkdb -> compute ring -> pipeline_worker -> output ring -> dada_dbnull` and never starts qtp senders, `rdma2dada`, or `vdif_unpack_worker`.
- Observation JSON and Resolved Plan remain the authority for headers, ring keys, block sizes, output format, and algorithm chain.
- A producer transfer contains an integer number of compute blocks; the output transfer contains the matching integer number of output blocks after applying the Resolved Plan contract.
- `dada_junkdb` is a coarse, whole-second token producer: it may deliver each second's allowance in a burst and block on the compute ring. GPU-only results therefore establish sustained pressure capacity and backpressure behavior, not smooth per-block arrival jitter or final astronomical real-time acceptance; `full` remains the final timing topology.
- The GPU producer header must be derived from `unpacked.header` but must rewrite `FILE_SIZE`, `TRANSFER_SIZE`, `BYTES_PER_SECOND`, and duration-dependent fields to the exact block-aligned junk transfer. Passing an unchanged compiler header to a differently sized junk transfer is invalid.
- Comparable tests load the latest passing profile. Explicit deviations require a non-empty experiment name and a complete field-by-field diff before resource creation.
- Formal performance acceptance is one warm-up plus three measured runs. Local/macOS tests are development evidence only.
- Successful suites retain shared configuration JSON, `summary.json`, `MANIFEST.sha256`, and one run JSON plus one evidence log per repetition. Failed repetitions add only the three documented debug artifacts.
- Git remains local. Do not commit or push until GPU-server acceptance passes and the user explicitly authorizes the Git action.
- HF and all remote testing remain paused until the user reports that HF is available.

---

## File Structure

- Create `scripts/task8c_profiles.py`: profile schema parsing, default application, SHA256 identity, and drift comparison.
- Create `scripts/task8c_artifacts.py`: process-ledger validation, evidence extraction, suite compaction, and retained-file manifest generation.
- Modify `scripts/task8c_rate_point.py`: topology orchestration, `dada_junkdb` producer construction, profile-aware CLI, and calls into the support modules.
- Create `config/testing/profiles/README.md`: accepted-profile provenance and bootstrap rules.
- Create `config/testing/profile.schema.json`: documented profile fields and types; validation remains implemented without a third-party JSON-schema dependency.
- Create after HF recovery and evidence extraction: `config/testing/profiles/qths1-receive-30gbps-v1.json` and `config/testing/profiles/qths1-unpack-30gbps-v1.json`.
- Create `tests/task8c_profiles_test.py`: profile loading, precedence, identity, and drift-gate tests.
- Create `tests/task8c_artifacts_test.py`: exact retained-file contract, process-ledger completeness, failure compaction, and manifest tests.
- Modify `tests/task8c_rate_point_test.py`: GPU producer topology, block/rate derivation, lifecycle, cleanup, and cross-topology regression tests.
- Create `include/rdma_dada/pipeline/worker_metrics.h`: structured block/service timing accumulator interface.
- Create `src/pipeline/worker_metrics.cpp`: timing aggregation and JSON serialization implementation.
- Modify `apps/pipeline_worker/main.cpp`: optional metrics output, block accounting, output-ring wait timing, and CUDA stage timing.
- Create `tests/pipeline_worker_metrics_test.cpp`: portable aggregation/serialization tests.
- Create `scripts/generate_source_manifest.py`: deterministic source-manifest generation without depending on Git on test hosts.
- Create `tests/generate_source_manifest_test.py`: allowlist/exclusion and stable ordering tests.
- Modify `CMakeLists.txt`: register the two focused Python test targets.
- Modify `docs/agents/testing.md`, `tests/README.md`, and `docs/PROJECT_STATUS.md`: commands, profile policy, compact JSON schema, and accepted/deferred status.
- Modify `.gitignore`: ignore repository-root `/tmp/` render/scratch output without ignoring system `/tmp` paths referenced by scripts.

---

### Task 1: Freeze the Four-Topology Contract

**Files:**
- Modify: `scripts/task8c_rate_point.py:54-94`
- Modify: `tests/task8c_rate_point_test.py`

**Interfaces:**
- Consumes: existing `PipelineTopology` and `pipeline_topology(stage)`.
- Produces: `PipelineTopology.input_kind: str` with values `network` or `junkdb`, and `required_process_roles(topology: PipelineTopology) -> tuple[str, ...]`.

- [x] **Step 1: Add failing topology tests**

Add assertions equivalent to:

```python
self.assertEqual(MODULE.pipeline_topology("receive").rings, ("raw",))
self.assertEqual(MODULE.pipeline_topology("unpack").rings, ("raw", "compute"))
self.assertEqual(MODULE.pipeline_topology("gpu").rings, ("compute", "output"))
self.assertEqual(MODULE.pipeline_topology("gpu").input_kind, "junkdb")
self.assertEqual(MODULE.pipeline_topology("full").rings,
                 ("raw", "compute", "output"))
self.assertFalse(MODULE.pipeline_topology("gpu").uses_network_senders)
```

Also assert the PASS-required roles:

```python
self.assertEqual(
    MODULE.required_process_roles(MODULE.pipeline_topology("gpu")),
    ("compute-ring", "output-ring", "dada_junkdb",
     "pipeline_worker", "output-consumer"),
)
```

- [x] **Step 2: Run the focused test and verify failure**

Run:

```bash
python3 -B tests/task8c_rate_point_test.py -q
```

Expected: failure because `input_kind` and `required_process_roles` do not exist.

- [x] **Step 3: Implement the explicit topology fields**

Extend `PipelineTopology` rather than inferring producers from stage-name conditionals. Define all four entries explicitly and return the role tuple from topology flags and ring names. Do not add a fifth public test mode.

- [x] **Step 4: Run the focused test**

Run the same command. Expected: all topology tests pass.

- [x] **Step 5: Review checkpoint**

Inspect `git diff --check` and the focused diff. Do not commit.

---

### Task 2: Implement Versioned Passing Profiles

**Files:**
- Create: `scripts/task8c_profiles.py`
- Create: `config/testing/profile.schema.json`
- Create: `config/testing/profiles/README.md`
- Create after HF evidence extraction: `config/testing/profiles/qths1-receive-30gbps-v1.json`
- Create after HF evidence extraction: `config/testing/profiles/qths1-unpack-30gbps-v1.json`
- Create: `tests/task8c_profiles_test.py`
- Modify: `scripts/task8c_rate_point.py:96-220,3922-4118`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:

```python
@dataclasses.dataclass(frozen=True)
class BaselineProfile:
    schema_version: int
    profile_id: str
    target_host: str
    pipeline_stage: str
    source_result: str
    runtime: dict[str, object]
    geometry: dict[str, int]
    sha256: str

def load_profile(path: pathlib.Path) -> BaselineProfile: ...
def apply_profile(request: RateRequest, profile: BaselineProfile) -> RateRequest: ...
def compare_profile(plan: RatePlan, profile: BaselineProfile) -> list[dict[str, object]]: ...
```

- Consumes: `RateRequest`, compiled `RatePlan`, and the current host/topology.
- CLI produces: `--baseline-profile PATH` and `--experiment-name NAME`. A normal formal run requires a baseline. A topology with no accepted baseline may run only with an explicit `--experiment-name bootstrap-<topology>-v1`; `preflight.json` records `baseline_status=BOOTSTRAP_CANDIDATE`, and the run cannot install its own profile.

- [x] **Step 1: Write profile parsing and validation tests**

Cover missing/unknown fields, duplicate CPU roles, wrong target host/topology, invalid SHA identity, non-positive queue/ring values, and a valid profile round trip.

The runtime profile must contain:

```json
{
  "receiver_poll_cpu": 13,
  "worker_cpu_list": "14,15,16,17,18,19",
  "sink_cpu_list": "20",
  "numa_node": 1,
  "receiver_poll_batch": 32,
  "receiver_wr_num": 1024,
  "unpack_start_delay_seconds": 1,
  "missing_wait_ms": 200.0,
  "station_skew_reserve_ms": 200.0
}
```

Fields not used by a topology are omitted, not set to `null`.

- [x] **Step 2: Verify the new tests fail**

Run:

```bash
python3 -B tests/task8c_profiles_test.py -q
```

Expected: import failure because `task8c_profiles.py` does not exist.

- [x] **Step 3: Implement the parser and immutable SHA identity**

Parse JSON with exact required/optional field sets, reject booleans where integers are required, canonicalize using sorted compact JSON, and compute SHA256 over the original file bytes. `apply_profile` fills unset runtime values; it must not override explicit CLI values.

- [x] **Step 4: Add drift comparison tests**

Assert an exact plan yields `[]`. Assert a changed CPU, WR depth, ring block count, window block count, preparation delay, or topology yields entries shaped as:

```json
{"field":"runtime.receiver_poll_cpu","baseline":13,"effective":12}
```

- [x] **Step 5: Implement fail-closed CLI behavior**

Make placement/queue/preparation CLI defaults distinguish “not supplied” from an explicit override. For `--execute` and formal `--preflight-only`:

1. load `--baseline-profile` before calling `RateRequest.validate()` or `compile_rate_plan()`;
2. fill unset request fields;
3. compile the Resolved Plan;
4. compare runtime and geometry;
5. continue only on an exact match, or when `--experiment-name` is non-empty and every diff is recorded;
6. when no accepted profile exists, allow only the explicit `bootstrap-<topology>-v1` experiment and record that there is no comparison baseline;
7. write profile path, profile SHA256, baseline status, experiment name, and full diff to suite `preflight.json` before resource creation.

An unnamed drift returns `TEST_RESULT=HARNESS_FAIL` at `CONFIG_PREFLIGHT` and creates no ring/process.

- [x] **Step 6: Add profile provenance rules, not fabricated profiles**

Document that an accepted profile is created only from a retained PASS run JSON whose process ledger and artifact manifest validate. While HF is unavailable, use test fixtures under temporary test directories; do not reconstruct accepted profile files from conversation summaries. Task 8 performs a read-only evidence extraction first, returns the exact values to the development worktree, and only then creates the receive/unpack profile files. Do not create a GPU/full “passing” profile before those new paths pass remotely.

- [x] **Step 7: Register and run tests**

Run:

```bash
python3 -B tests/task8c_profiles_test.py -q
python3 -B tests/task8c_rate_point_test.py -q
git diff --check
```

Expected: all pass.

- [x] **Step 8: Review checkpoint**

Confirm dry-run may display a profile without remote access, while execute/preflight cannot silently use scheduler/default placement. Do not commit.

---

### Task 3: Replace Single-Block GPU Input with `dada_junkdb`

**Files:**
- Modify: `scripts/task8c_rate_point.py:1406-1557,1951-1971,2815-2844`
- Modify: `tests/task8c_rate_point_test.py`

**Interfaces:**
- Produces:

```python
@dataclasses.dataclass(frozen=True)
class GpuJunkInputPlan:
    blocks_per_second: int
    block_count: int
    total_bytes: int
    bytes_per_second: int
    megabytes_per_second: str
    duration_seconds: int
    input_header_bytes: bytes

def derive_gpu_junk_input(plan: RatePlan) -> GpuJunkInputPlan: ...
```

- Consumes: `plan.compute_block_bytes`, requested payload rate, duration, compute key, and compiler-generated `unpacked.header`.
- Produces a GPU input header whose `FILE_SIZE` and `TRANSFER_SIZE` equal `total_bytes`, whose `BYTES_PER_SECOND` equals the block-aligned actual rate, and whose ATFP/CI8/config/geometry identity fields remain byte-for-byte compatible with the Resolved Plan.

- [x] **Step 1: Write block/rate derivation tests**

For each target rate and geometry, assert:

```python
junk.blocks_per_second == ceil(target_bytes_per_second /
                                plan.compute_block_bytes)
junk.bytes_per_second == junk.blocks_per_second * plan.compute_block_bytes
junk.block_count == junk.blocks_per_second * junk.duration_seconds
junk.total_bytes == junk.block_count * plan.compute_block_bytes
junk.bytes_per_second >= target_bytes_per_second
```

Reject non-integral duration in the first version because the installed
`dada_junkdb -t` contract is whole seconds. Do not reject an arbitrary target
because of rounding: record the actual block-aligned rate, which is always at
least the configured target.

Also assert the generated header has:

```python
self.assertEqual(header["FILE_SIZE"], junk.total_bytes)
self.assertEqual(header["TRANSFER_SIZE"], junk.total_bytes)
self.assertEqual(header["BYTES_PER_SECOND"], junk.bytes_per_second)
self.assertEqual(header["DATA_STAGE"], "UNPACKED")
self.assertEqual(header["ORDER"], "ATFP")
```

- [x] **Step 2: Verify the tests fail**

Run `python3 -B tests/task8c_rate_point_test.py -q`.

Expected: failure because `GpuJunkInputPlan` does not exist.

- [x] **Step 3: Implement block-aligned rate derivation**

Compute `ceil(target_bytes_per_second / compute_block_bytes)` blocks for each
whole second, then multiply by duration. Use Decimal arithmetic and decimal
string formatting. Generate `gpu-input.header` from the compiler header,
replace the transfer/rate fields above, and serialize exactly 4096 bytes.

- [x] **Step 4: Add bundle-construction tests**

Assert GPU mode:

- resolves `dada_db`, `dada_junkdb`, and the selected output consumer only;
- creates compute and output rings only;
- starts output consumer, then `pipeline_worker`, then `dada_junkdb`;
- passes compute key, the Phase-0-verified decimal-MB/s option, `-t <seconds>`, `-b <block-aligned-total>`, `-z`, and `gpu-input.header`;
- contains no qtp, sender, receiver, NIC capture, unpack worker, `dada_diskdb`, or generated `input.dada` artifacts.

- [x] **Step 5: Implement the GPU producer bundle**

Delete `make_compute_input.py`, `input.dada`, and the `dada_diskdb` requirement from GPU mode. Use the installed PSRDADA binary only after Phase 0 resolves its absolute path and confirms the rate-option semantics. The inspected PSRDADA source currently maps `-R` to decimal 1,000,000 B/s even though its help text names the units inconsistently; encode this as an observed capability rather than trusting the help label. Keep the product code independent of PSRDADA installation layout.

- [x] **Step 6: Add lifecycle and validation tests**

Fake-backend tests must prove:

- producer early exit stops `pipeline_worker` and output consumer;
- pipeline worker failure stops the producer;
- producer bytes equal the planned block-aligned transfer;
- the producer's one-second burst behavior and actual elapsed rate are reported explicitly rather than described as smooth pacing;
- output consumer reaches EOD and exits zero;
- `gpu.completed=true` requires producer, worker, and consumer success;
- cleanup removes exactly compute/output rings and owned processes.

- [x] **Step 7: Run local controller tests three times**

Run:

```bash
for i in 1 2 3; do python3 -B tests/task8c_rate_point_test.py -q || exit 1; done
git diff --check
```

Expected: three clean passes and no generated file outside temporary test directories.

- [x] **Step 8: Review checkpoint**

Confirm `aggregate-gbps` now controls compute-ring producer pressure in GPU mode and is not reported as network sender throughput. Do not commit.

---

### Task 4: Add GPU Worker Service Metrics

**Files:**
- Create: `include/rdma_dada/pipeline/worker_metrics.h`
- Create: `src/pipeline/worker_metrics.cpp`
- Modify: `apps/pipeline_worker/main.cpp`
- Create: `tests/pipeline_worker_metrics_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces `pipeline_worker RESOLVED_OBSERVATION.json --metrics-json FILE` while retaining the existing one-argument invocation.
- Produces an atomic JSON summary with input/output blocks and bytes, total transfer elapsed time, per-block total service time, output-ring acquisition wait, and CUDA H2D/algorithm-chain/D2H elapsed time.

- [x] **Step 1: Write portable metrics tests**

Test zero-block rejection, checked counter addition, min/max/mean calculation, percentile-bin ordering, JSON field names, and atomic finalization. Use deterministic integer nanosecond fixtures.

- [x] **Step 2: Verify the new test fails**

Build and run `pipeline_worker_metrics_test`; expected failure is the missing module.

- [x] **Step 3: Implement the metrics module**

Keep one deep interface: `RecordBlock(const WorkerBlockMeasurement&)` and `WriteJson(path)`. Do not expose histogram storage or JSON formatting to `main.cpp`.

- [x] **Step 4: Instrument the worker without changing algorithm results**

Measure output-ring acquisition with `std::chrono::steady_clock`. On CUDA builds, record events on the existing stream around H2D, conversion+module chain, and D2H; collect elapsed values after the existing per-block stream synchronization. Record total block service and byte counts for CPU and CUDA paths. Metrics failure is a harness/output failure and must not silently change numerical output.

- [x] **Step 5: Add CLI and failure-path tests**

Verify legacy invocation still works, metrics output is optional, a supplied metrics path is written on success and product failure, and no partial `.tmp` file is treated as authoritative.

- [x] **Step 6: Run local tests**

Run the portable metrics test and existing pipeline-worker CPU tests. CUDA timing is deferred to Task 8 server acceptance.

- [x] **Step 7: Review checkpoint**

Confirm instrumentation does not introduce a second CUDA stream or an additional unconditional device synchronization. Do not commit.

---

### Task 5: Record a Complete Process Ledger Before Compaction

**Files:**
- Create: `scripts/task8c_artifacts.py`
- Create: `tests/task8c_artifacts_test.py`
- Modify: `scripts/task8c_rate_point.py:1035-1075,1406-1525,3261-3455,3566-3746`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:

```python
def required_roles_for_result(topology: str, stages: list[dict[str, object]]) -> tuple[str, ...]: ...
def validate_process_ledger(entries: list[dict[str, object]], required_roles: tuple[str, ...]) -> None: ...
def compact_suite_run(suite_root: pathlib.Path, run_id: str,
                      result: dict[str, object]) -> dict[str, object]: ...
```

- Each process entry has exact keys:

```json
{
  "host":"qths1",
  "role":"pipeline_worker",
  "argv":["/absolute/path/pipeline_worker","/tmp/.../resolved_observation.json"],
  "env":{"CUDA_VISIBLE_DEVICES":"0"},
  "cpu_affinity":[21],
  "numa_node":1,
  "thread_mapping":[],
  "binary_sha256":"...",
  "config_sha256":"...",
  "pid":1234,
  "started_utc":"...",
  "ended_utc":"...",
  "returncode":0
}
```

- [x] **Step 1: Write ledger validation tests**

Create PASS fixtures for all four topologies. Remove each required role and each required field in turn and assert validation fails before the verbose directory is deleted. Early failures require only the processes whose start states appear in `state-history.jsonl`.

- [x] **Step 2: Verify the tests fail**

Run `python3 -B tests/task8c_artifacts_test.py -q`.

Expected: import failure because `task8c_artifacts.py` does not exist.

- [x] **Step 3: Implement process runtime recording**

Extend the existing supervisor so every qths process writes one atomic JSON lifecycle record containing planned identity plus PID/start/end/exit. Record ring owners, `rdma2dada`, unpack coordinator process, `dada_junkdb`, `pipeline_worker`, and consumers. Capture `/proc/<pid>/status` affinity/NUMA evidence immediately after start, before the process can exit. Merge the existing sender runtime ledger without losing Station, SSH argv, or combined output.

- [x] **Step 4: Record effective placement and identity**

Populate taskset CPU, NUMA node, unpack thread-role mapping, binary SHA256, and Resolved Plan/config SHA256 from the preflight manifest. Record only explicit process environment overrides and a fixed performance-relevant allowlist (`CUDA_VISIBLE_DEVICES`, `OMP_NUM_THREADS`, `LD_LIBRARY_PATH`, and `TASK8C_*`); never serialize the inherited full environment or credentials. Do not reconstruct these fields from log text during compaction.

- [x] **Step 5: Implement fail-closed compaction**

For a PASS result, validate all topology-required roles and fields. If validation fails, change the outcome to `HARNESS_FAIL`, preserve the first ledger error in debug artifacts, perform scoped cleanup, and retain enough verbose source evidence for diagnosis. Delete the verbose run directory only after run JSON, evidence log, and their SHA256 values are durable.

- [x] **Step 6: Register and run tests**

Run:

```bash
python3 -B tests/task8c_artifacts_test.py -q
python3 -B tests/task8c_rate_point_test.py -q
git diff --check
```

Expected: all pass.

- [x] **Step 7: Review checkpoint**

Inspect one fake PASS run and verify every launched process is represented exactly once. Do not commit.

---

### Task 6: Enforce the Compact Suite Artifact Contract

**Files:**
- Modify: `scripts/task8c_artifacts.py`
- Modify: `tests/task8c_artifacts_test.py`
- Modify: `scripts/task8c_rate_point.py:3749-3915,4084-4118`

**Interfaces:**
- Consumes: validated process ledger, structured statistics, state history, cleanup result, and source logs.
- Produces the exact PASS file tree documented in `docs/agents/testing.md` and only the three additional debug files on failure.

- [x] **Step 1: Write exact file-set tests**

For a warm-up plus three measured PASS suite, assert the retained relative paths are exactly:

```text
MANIFEST.sha256
observation.json
preflight.json
resolved_observation.json
summary.json
runs/warmup-01.evidence.log
runs/warmup-01.json
runs/measured-01.evidence.log
runs/measured-01.json
runs/measured-02.evidence.log
runs/measured-02.json
runs/measured-03.evidence.log
runs/measured-03.json
```

For failure, add only `debug/<run-id>/failed-command.json`, `failed-process.log`, and `resource-snapshot.json`.

- [x] **Step 2: Write evidence integrity tests**

Assert evidence contains unmodified receiver/unpack/GPU/junkdb final summaries, `FILE_SIZE`, `close: wrote`, EOD, warning/error, process exit and cleanup lines prefixed by source role, excludes progress spam, and its SHA matches the run JSON. Assert `sha256sum -c MANIFEST.sha256` semantics over every retained file except the manifest itself.

- [x] **Step 3: Implement shared suite preflight**

Formal execution writes profile identity/diff, compiler validation, binary identities, topology, geometry, and Phase 0 facts once to `preflight.json`. Repetitions reference it by SHA. `--preflight-only` may create a standalone diagnostic directory, but formal execution must not create a sibling `*-preflight` directory.

- [x] **Step 4: Implement exact compaction and atomic writes**

Write all JSON through temporary-file replacement, fsync the retained run JSON/evidence before deleting verbose inputs, and regenerate `MANIFEST.sha256` after every repetition and final `summary.json` update.

- [x] **Step 5: Test reporting-task consumption**

Add a test that reads only `summary.json`, each referenced run JSON, and each referenced evidence file; it must recover topology, effective profile/diff, result, rates, counters, process ledger, stage timeline, cleanup, and evidence SHA without opening deleted logs or bundles.

- [x] **Step 6: Run compact-artifact tests three times**

Run:

```bash
for i in 1 2 3; do python3 -B tests/task8c_artifacts_test.py -q || exit 1; done
python3 -B tests/task8c_rate_point_test.py -q
git diff --check
```

Expected: all pass and temporary fixture directories self-remove.

- [x] **Step 7: Review checkpoint**

Confirm failure information and cleanup status are independent and that compaction never replaces the original test result with `CLEANED`. Do not commit.

---

### Task 7: Update Documentation and Local Acceptance

**Files:**
- Modify: `.gitignore`
- Modify: `docs/agents/testing.md`
- Modify: `tests/README.md`
- Modify: `docs/PROJECT_STATUS.md`
- Modify: `config/testing/atfp-throughput-source-manifest.sha256`
- Create: `scripts/generate_source_manifest.py`
- Create: `tests/generate_source_manifest_test.py`

**Interfaces:**
- Documents the four commands, profile requirements, GPU input-rate meaning, retained JSON schema, and deferred remote status.

- [x] **Step 1: Update `.gitignore`**

Add `/tmp/` under generated local artifacts. Confirm no source file is ignored:

```bash
git check-ignore -v tmp/pdfs/example.png
git check-ignore include/rdma_dada/config/gpu_pipeline_budget.h
git check-ignore src/config/gpu_pipeline_budget.cpp
```

Expected: only the first path is ignored.

- [x] **Step 2: Document exact controller commands**

Document:

- `receive`: network senders -> `rdma2dada` -> raw ring -> `dada_dbnull`;
- `unpack`: network senders -> `rdma2dada` -> raw ring -> unpack -> compute ring -> `dada_dbnull`;
- `gpu`: `dada_junkdb` -> compute ring -> GPU worker -> output ring -> `dada_dbnull`;
- `full`: network senders -> receive -> unpack -> GPU worker -> output ring -> `dada_dbnull`.

State that GPU `aggregate-gbps` is compute-ring payload pressure, whereas network modes use aggregate sender payload rate.

- [x] **Step 3: Correct status claims**

Mark profile enforcement, `dada_junkdb` pressure, and new compact ledger as locally implemented only after local gates pass. Mark GPU-server acceptance as `ENV_BLOCKED/HF unavailable` until the deferred remote task completes. Do not call the feature accepted from local tests.

- [x] **Step 4: Add a deterministic manifest generator**

Generate from an explicit repository allowlist covering `CMakeLists.txt`, `AGENTS.md`, top-level README, and product/config/test/documentation files under `apps/`, `config/`, `doc/`, `docs/`, `include/`, `modules/`, `scripts/`, `src/`, `tests/`, and `tools/`. Exclude `.git`, `.agents`, `.claude`, `agent`, `build*`, result/data directories, `/tmp/`, caches, and the manifest file itself. Sort POSIX relative paths and hash file bytes; do not invoke Git so the rule is testable before a commit.

Test inclusion of newly created source files, exclusion of scratch/generated files, stable ordering, and repeatable output.

- [x] **Step 5: Regenerate the complete source manifest**

Run the new versioned generator, then verify locally:

```bash
shasum -a 256 -c config/testing/atfp-throughput-source-manifest.sha256
```

Expected: every entry passes.

- [x] **Step 6: Run the complete local gate**

Run:

```bash
python3 -B tests/task8c_profiles_test.py -q
python3 -B tests/task8c_artifacts_test.py -q
python3 -B tests/generate_source_manifest_test.py -q
for i in 1 2 3; do python3 -B tests/task8c_rate_point_test.py -q || exit 1; done
git diff --check
shasum -a 256 -c config/testing/atfp-throughput-source-manifest.sha256
```

Expected: all pass.

- [ ] **Step 7: Record the reporting interface**

Record that `总结成文` reads the development-maintained catalog on demand. Its
stable interface is `summary.json` plus referenced run JSON/evidence files;
per-test notifications are not part of the workflow.

- [x] **Step 8: Stop at the local checkpoint**

Report local results and wait. Do not contact HF, create remote resources, commit, or push.

---

### Task 8: Deferred GPU-Server Acceptance After HF Recovery

**Files:**
- No product-code edits in the testing task.
- Retain results under `/home/user/wy/task8c-results/` using the compact contract.

**Interfaces:**
- Consumes: user notification that HF is reachable, current source manifest, explicit Release binary directories, Observation JSON, and topology profile.
- Produces: callback with independent `TEST_RESULT`, `CLEANUP_RESULT`, and result notification status.

- [ ] **Step 1: Wait for explicit HF recovery notification**

Do not probe HF before the user reports recovery.

- [ ] **Step 2: Perform Phase 0 before altering the harness**

From the designated controller host, verify exact paths and options for `dada_db`, `dada_junkdb`, and `dada_dbnull`; record `dada_junkdb -h`, binary SHA256, `ldd`, actual `-r`/`-R` byte-unit behavior, whole-second token/burst behavior, PSRDADA paths, GPU/CUDA, CPU/NUMA, memory, NIC, clock, and shell semantics. If the installed binary's required behavior cannot be established, return `ENV_BLOCKED` before formal runs.

- [ ] **Step 3: Extract accepted network profiles before formal execution**

Read the retained PASS receive/unpack run JSON and evidence manifests without starting processes. Return the exact CPU/NUMA/thread, queue, ring/window, preparation, config identity, source-result path, and artifact SHA values to the development task. Create the two versioned profile files locally, regenerate the source manifest, and mirror again before running the profile-gated formal commands.

- [ ] **Step 4: Mirror and run server unit gates**

Mirror the complete worktree through HF using the source manifest, build a fresh CUDA Release directory, then run the profile, artifact, controller, GPU numerical, and registered CTest gates three times.

- [ ] **Step 5: Revalidate known passing network profiles**

Run `receive` and `unpack` using their versioned profiles. Start with a short functional repetition; if configuration and identities match, perform the required warm-up plus three measured repetitions. Any profile diff without an experiment name must fail before ring creation.

- [ ] **Step 6: Accept GPU-only pressure behavior**

Run `gpu` first at 1 Gbps for one warm-up plus three measured repetitions. Require exact junkdb input bytes/block count, pipeline-worker completion, exact output transfer accounting, output EOD, no sustained ring growth, complete process ledger, worker total/per-stage service metrics, compact artifact manifest, and clean scoped cleanup. Report junkdb's one-second burst behavior explicitly. Then test 5, 10, 20, and 30 Gbps in order, stopping at the first failure; do not call a single run stable.

- [ ] **Step 7: Run one full-chain regression**

Using the accepted full topology profile or an explicitly named bootstrap experiment, run low-rate `full` for one warm-up plus three measured repetitions. Require sender -> receiver -> unpack -> GPU -> output reconciliation and the same compact artifact contract.

- [ ] **Step 8: Return results and wait for Git authorization**

The GPU testing task sends the complete callback to the development task. If all requested gates pass, remind the user that the changes are ready to commit. Do not commit or push until the user explicitly authorizes it.

---

## Self-Review

- Spec coverage: GPU rate control and header consistency, coarse-pacing limitation, worker service metrics, four topologies, profile loading/drift, complete process ledger, compact PASS/failure file sets, deterministic source manifest, local gates, deferred server gates, cleanup, and reporting-task consumption are each assigned to a task.
- Placeholder scan: no implementation step depends on an unspecified command, path, type, or acceptance result. GPU/full profiles are intentionally not fabricated before remote acceptance; their bootstrap path is an explicit named experiment.
- Type consistency: `BaselineProfile`, `GpuJunkInputPlan`, topology role derivation, and artifact APIs are defined before downstream use.
- Remote boundary: Tasks 1–7 are local development only. Task 8 cannot start until the user explicitly reports HF recovery.
