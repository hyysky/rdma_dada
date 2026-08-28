# Test Result Catalog and Cross-Task Handoff Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Import compact remote test suites into an ignored local evidence store, generate deterministic JSON/CSV indexes, support selective queries, and hand new suite paths to both development and report-writing tasks.

**Architecture:** Each compact suite remains immutable and authoritative. A versioned Python tool validates/imports suites, derives a rebuildable catalog, filters entries, and explicitly promotes selected evidence into a tracked publication index. The testing task copies compact artifacts but never edits table rows manually; `总结成文` reads only query-selected suite JSON.

**Tech Stack:** Python 3.8 standard library, unittest, CMake/CTest, JSON, CSV, SHA256 manifests.

**Spec:** `docs/superpowers/specs/2026-08-26-test-result-catalog-and-handoff-design.md`

## Global Constraints

- Suite `summary.json`, `preflight.json`, `runs/*.json`, evidence logs, and manifest remain authoritative; catalogs are derived.
- Never create catalog fields from chat history or unretained remote logs.
- `/test-results/` remains ignored by Git.
- Import validates in a temporary sibling and atomically renames into `test-results/suites/<suite-id>`.
- Identical re-import is idempotent; a conflicting suite ID fails closed.
- Test, cleanup, import, development notification, and summary notification statuses remain independent.
- The catalog tool runs no SSH, Git, remote cleanup, or credential command.
- New Python code supports Python 3.8; avoid `X | Y` runtime annotations.
- Do not commit or push without explicit user authorization.

---

### Task 1: Define Schema, Ignore Rule, and RED Fixture

**Files:**
- Create: `config/testing/test-result-catalog.schema.json`
- Create: `tests/task8c_catalog_test.py`
- Modify: `.gitignore`

**Interfaces:**
- Consumes: compact suite contract in `scripts/task8c_artifacts.py`.
- Produces: schema version 1 and `make_suite_fixture(...)` for later tests.

- [ ] **Step 1: Ignore runtime evidence**

Append:

```gitignore
# Imported compact test evidence and generated local catalogs
/test-results/
```

- [ ] **Step 2: Define catalog schema**

Create a Draft 2020-12 schema requiring `schema_version`, `generated_utc`,
`suite_count`, and `suites`. Each entry must use topology
`receive|unpack|gpu|full`, positive finite rate/duration, nonnegative repetition
counts, required result/cleanup states, 64-character lowercase SHA fields, and
a relative `result_path` below `suites/`. Set `additionalProperties: false` at
the catalog and entry levels.

- [ ] **Step 3: Create a real compact-suite fixture**

In `tests/task8c_catalog_test.py`, add:

```python
def make_suite_fixture(root, suite_id="unpack-30Gbps-60s-20260826T120000Z",
                       topology="unpack", test_result="PASS",
                       cleanup_result="PASS"):
    suite = pathlib.Path(root) / suite_id
    (suite / "runs").mkdir(parents=True)
    request = {
        "aggregate_gbps": 30.0,
        "duration_seconds": 60.0,
        "pipeline_stage": topology,
        "baseline_profile_path": "config/testing/profiles/qths1-unpack-30gbps-v1.json",
        "baseline_profile_sha256": "b" * 64,
        "receiver_poll_cpu": 13,
        "worker_cpu_list": "14,15,16,17,18,19" if topology in ("unpack", "full") else None,
        "gpu_worker_cpu": 21 if topology in ("gpu", "full") else None,
        "sink_cpu_list": "20",
        "numa_node": 1,
        "receiver_poll_batch": 32,
        "receiver_wr_num": 1024,
        "unpack_start_delay_seconds": 1,
    }
    plan = {
        "pipeline_stage": topology,
        "aggregate_gbps": 30.0,
        "duration_seconds": 60.0,
        "config_id": "c" * 64,
        "geometry_id": "d" * 64,
        "raw_block_bytes": 52838400,
        "raw_ring_blocks": 16,
        "compute_block_bytes": 52428800,
        "compute_ring_blocks": 8,
        "output_block_bytes": 209715200,
        "output_ring_blocks": 8,
        "window_groups": 198400,
        "reorder_horizon_groups": 96000,
        "profile_evidence": {"profile_id": "qths1-unpack-30gbps-v1"},
    }
    summary = {
        "TEST_RESULT": test_result,
        "suite_id": suite_id,
        "request": request,
        "warmup_count": 0,
        "measured_count": 1,
        "runs": [{"role": "measured", "index": 1,
                  "run_id": "measured-01", "TEST_RESULT": test_result,
                  "CLEANUP_RESULT": cleanup_result,
                  "result_path": "runs/measured-01.json"}],
        "actual_aggregate_gbps": {
            "median": 29.9999, "minimum": 29.9998,
            "maximum": 30.0, "spread": 0.0002,
        } if test_result == "PASS" else None,
    }
    preflight = {
        "created_utc": "2026-08-26T12:00:00+00:00",
        "plan": plan,
        "config": {"binary_sha": {"qths1:rdma2dada": "e" * 64}},
    }
    run = {
        "TEST_RESULT": test_result,
        "cleanup": {"CLEANUP_RESULT": cleanup_result},
        "failure": None if test_result == "PASS" else {
            "stage": "COLLECTING", "classification": test_result,
            "exit_code": 1, "stderr": "count mismatch",
        },
        "statistics": {},
    }
    (suite / "summary.json").write_text(json.dumps(summary) + "\n")
    (suite / "preflight.json").write_text(json.dumps(preflight) + "\n")
    (suite / "runs" / "measured-01.json").write_text(json.dumps(run) + "\n")
    (suite / "runs" / "measured-01.evidence.log").write_text("receiver: summary\n")
    task8c_artifacts.write_manifest(suite)
    return suite
```

- [ ] **Step 4: Write and run the first RED test**

```python
def test_rebuild_empty_root_creates_catalog(self):
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        catalog.rebuild_catalog(root, generated_utc="2026-08-26T00:00:00Z")
        value = json.loads((root / "catalog.json").read_text())
        self.assertEqual(value["suite_count"], 0)
        self.assertEqual(value["suites"], [])
        self.assertTrue((root / "catalog.csv").is_file())
        self.assertTrue((root / "MANIFEST.sha256").is_file())
```

Run `python3 -B tests/task8c_catalog_test.py -q`. Expected: import failure
because `scripts/task8c_catalog.py` does not exist.

---

### Task 2: Implement Strict Validation and Atomic Import

**Files:**
- Create: `scripts/task8c_catalog.py`
- Modify: `tests/task8c_catalog_test.py`

**Interfaces:**
- Produces: `validate_suite(path) -> dict` and `import_suite(results_root, source_dir, source_host, remote_suite_root, source_manifest_sha256) -> pathlib.Path`.

- [ ] **Step 1: Add RED import tests**

Test successful import and `origin.json`, identical idempotent re-import,
conflicting same-ID rejection, manifest mismatch, missing `preflight.json`,
path-traversal suite IDs, invalid source-manifest SHA, and cleanup of
`.import-*` after injected failure.

```python
target = catalog.import_suite(
    results_root, source_suite, "qths1", "/home/user/wy/results/suite",
    "a" * 64
)
self.assertEqual(target, results_root / "suites" / source_suite.name)
self.assertTrue((target / "origin.json").is_file())
self.assertFalse(any(results_root.glob(".import-*")))
```

- [ ] **Step 2: Run RED**

Run the new tests. Expected: missing `validate_suite`/`import_suite` failures.

- [ ] **Step 3: Implement JSON and manifest helpers**

Create these interfaces:

```python
HEX64 = re.compile(r"^[0-9a-f]{64}$")

def load_json_object(path):
    value = json.loads(pathlib.Path(path).read_text())
    if not isinstance(value, dict):
        raise ValueError("JSON artifact must contain an object: %s" % path)
    return value

def sha256_file(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()

def parse_manifest(root):
    # Return relative-path -> SHA. Reject absolute paths, '..', duplicates,
    # malformed SHA values, missing files, symlinks, and self-reference.

def verify_manifest(root):
    # Verify every listed digest. origin.json is local provenance and is
    # covered by the root catalog manifest rather than the remote suite manifest.
```

- [ ] **Step 4: Implement suite validation**

Require `summary.json`, `preflight.json`, `MANIFEST.sha256`, nonempty `runs`,
`summary.suite_id == path.name`, valid topology, every summary `result_path`
below `runs/`, every referenced run JSON and matching evidence log, and exact
manifest verification. Return loaded summary, preflight, and runs without
mutating files.

- [ ] **Step 5: Implement atomic import**

Use `results_root/.import-<uuid>`, `shutil.copytree`, validation before and after
copy, atomic `origin.json` containing the supplied development source-manifest
SHA, then `Path.rename()` into `suites/<suite-id>`. On
error remove only that temporary directory. If the target exists, compare and
verify source/target suite manifest SHA; return unchanged for identical bytes
or raise `ValueError("conflicting suite id")`. Identical re-import must also
match origin host, remote root, and source-manifest SHA. Never delete
`source_dir`. At this task boundary import returns the immutable suite; Task 3
adds catalog rebuild/verification after publication.

- [ ] **Step 6: Run GREEN**

Run `tests/task8c_catalog_test.py` and `tests/task8c_artifacts_test.py`; require
exit 0.

---

### Task 3: Build Deterministic Catalog JSON and CSV

**Files:**
- Modify: `scripts/task8c_catalog.py`
- Modify: `tests/task8c_catalog_test.py`

**Interfaces:**
- Produces: `derive_catalog_entry(suite_root) -> dict`, `validate_catalog_value(value) -> None`, `rebuild_catalog(results_root, generated_utc=None) -> dict`, and `verify_catalog(results_root) -> None`.

- [ ] **Step 1: Add RED extraction tests**

Generate all four topologies and require exact module lists:

```python
EXPECTED_MODULES = {
    "receive": ["rdma2dada", "dada_dbnull"],
    "unpack": ["rdma2dada", "vdif_unpack_worker", "dada_dbnull"],
    "gpu": ["dada_junkdb", "pipeline_worker", "dada_dbnull"],
    "full": ["rdma2dada", "vdif_unpack_worker", "pipeline_worker", "dada_dbnull"],
}
```

Assert rate, duration, repetition counts, aggregate rate, independent test and
cleanup results, first failure, source/config/geometry/profile/binary identity,
CPU/NUMA and queue fields, topology-relevant ring/window geometry, relative
result path, summary SHA, and suite-manifest SHA.

- [ ] **Step 2: Add RED deterministic rebuild tests**

Import suites in reverse order and rebuild twice with a fixed `generated_utc`.
Require byte-identical JSON/CSV, ordering by `(started_utc, suite_id)`, unique
IDs, exact suite count, and the stable CSV header from the spec.

- [ ] **Step 3: Implement normalized extraction**

```python
def summary_request(summary):
    value = summary.get("request", summary.get("plan"))
    if not isinstance(value, dict):
        raise ValueError("suite summary has no request or plan")
    return value

def suite_cleanup_result(summary):
    # PASS only when every listed run cleanup is PASS; return the first
    # non-PASS cleanup state without changing TEST_RESULT.

def first_failure(summary, runs):
    # Follow summary.runs order and return run_id, stage, classification,
    # exit_code, and a one-line evidence-backed message, or None.
```

Use request/summary for topology, target rate, duration, repetitions, and
runtime placement. Use `preflight.plan` for checked geometry/config IDs and
`preflight.config.binary_sha` for binaries. Use `origin.json` for source
manifest/provenance. Reject conflicting summary/preflight values.

- [ ] **Step 4: Implement atomic rebuild**

Write temporary JSON and CSV beside their targets, close/fsync, and replace
with `os.replace`. Use `csv.DictWriter`; encode module lists with `+` and null
CSV values as empty cells. Generate a small root manifest covering
`catalog.json`, `catalog.csv`, and each suite's `MANIFEST.sha256` and
`origin.json` identities rather than rehashing every evidence file.

- [ ] **Step 5: Implement verification and failure preservation**

Verify root manifest, schema version, suite count/order/uniqueness, exact result
paths, summary/manifest SHA, all immutable suite directories, and path
containment. Corrupt one suite after a valid rebuild and prove a failed rebuild
leaves previous JSON/CSV bytes unchanged.

- [ ] **Step 6: Complete import-to-catalog publication**

After the suite rename in `import_suite()`, call
`rebuild_catalog(results_root)` and `verify_catalog(results_root)`. If catalog
publication fails, retain the already validated immutable suite as unindexed
evidence and return a catalog-import failure. The next `rebuild` must discover
and index it; never delete valid suite evidence to restore an older index.

- [ ] **Step 7: Run GREEN**

Run all catalog tests and require exit 0.

---

### Task 4: Implement Selective Queries

**Files:**
- Modify: `scripts/task8c_catalog.py`
- Modify: `tests/task8c_catalog_test.py`

**Interfaces:**
- Produces: `query_catalog(catalog_value, suite_id=None, topology=None, rate_gbps=None, test_result=None, cleanup_result=None, started_from=None, started_to=None, baseline_profile_id=None, latest=None) -> list`.

- [ ] **Step 1: Add RED query tests**

Create suites varying topology, rate, time, profile, test result, and cleanup.
Require AND-combined filters, exact stored numeric rate matching, inclusive date
bounds, newest `latest=N` selection returned chronologically, empty-selection
success, and argument failure for invalid dates or nonpositive latest.

- [ ] **Step 2: Add RED output tests**

Invoke CLI `query --format json` and `query --format paths`. Require complete
entry JSON or one absolute local suite path per line. Confirm query reads no
evidence log.

- [ ] **Step 3: Implement five CLI subcommands**

Create `import`, `rebuild`, `query`, `verify`, and `promote` subparsers. Query
accepts:

```text
--results-root --suite-id --topology --rate-gbps --result --cleanup-result
--started-from --started-to --baseline-profile-id --latest --format
```

Call `verify_catalog()` before query. Return 0 for an empty selection, 2 for
invalid input/catalog, and never mutate suite/catalog files.

- [ ] **Step 4: Run GREEN**

Run catalog tests and one CLI query against a temporary fixture.

---

### Task 5: Implement Explicit Evidence Promotion

**Files:**
- Modify: `scripts/task8c_catalog.py`
- Modify: `tests/task8c_catalog_test.py`
- Create: `docs/results/README.md`
- Create: `docs/results/accepted-results.json`

**Interfaces:**
- Produces: `promote_suite(results_root, suite_id, accepted_output) -> dict`.

- [ ] **Step 1: Add RED promotion tests**

Require exact entry copy without logs, `promoted_utc`, idempotent same suite/SHA,
conflicting duplicate rejection, unknown/unverified suite rejection, and no Git
subprocess. Explicitly prove a failed performance boundary may be promoted as
accepted evidence; “accepted” does not mean product PASS.

- [ ] **Step 2: Create the tracked empty evidence index**

```json
{
  "schema_version": 1,
  "results": []
}
```

Document the evidence meaning and explicit-user-approval requirement in
`docs/results/README.md`.

- [ ] **Step 3: Implement atomic promotion**

Load/validate the accepted file, append `{catalog_entry, promoted_utc}`, sort by
suite start time/ID, and atomically replace it. CLI restricts output to
repository `docs/results`; the library accepts temporary test paths. Run no Git
command.

- [ ] **Step 4: Run GREEN**

Run promotion tests, prove tests leave the repository accepted file unchanged,
and run `git diff --check`.

---

### Task 6: Register Tests and Document Development-Owned Handoff

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `tests/README.md`
- Modify: `docs/agents/testing.md`
- Modify: `docs/PROJECT_STATUS.md`
- Modify: `config/testing/atfp-throughput-source-manifest.sha256`

**Interfaces:**
- Produces: registered `task8c_catalog_test` and exact operational contract in which `GPU服务器代码测试` reports to the development task and the development task maintains the catalog consumed on demand by `总结成文`.

- [ ] **Step 1: Register CTest**

Inside `if(Python3_Interpreter_FOUND)` add:

```cmake
add_test(
    NAME task8c_catalog_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/task8c_catalog_test.py)
```

- [ ] **Step 2: Document CLI and authority order**

Document all five commands, local paths, immutable import, JSON/CSV derivation,
query behavior, promotion boundary, and when evidence logs may be read.

- [ ] **Step 3: Document the single test-result callback**

After remote cleanup and compaction, notify the development task with:

```text
RESULT_NOTIFICATION=<PASS|FAIL>
SUITE_ID=<suite-id>
TEST_TOPOLOGY=<receive|unpack|gpu|full>
TEST_RESULT=<result>
CLEANUP_RESULT=<result>
REMOTE_SUITE_PATH=<absolute compact-suite path>
SUITE_MANIFEST_SHA256=<sha256>
```

The development task then imports/verifies the suite and records
`CATALOG_IMPORT_RESULT` separately. `总结成文` reads the maintained catalog on
demand and receives no per-test callback.

- [ ] **Step 4: Update status and manifest**

Mark catalog local/server status accurately. Regenerate and verify:

```bash
python3 scripts/generate_source_manifest.py --root . \
  --output config/testing/atfp-throughput-source-manifest.sha256
shasum -a 256 -c config/testing/atfp-throughput-source-manifest.sha256
```

---

### Task 7: Local Acceptance and Review

**Files:**
- Verify all Task 1–6 files

**Interfaces:**
- Produces: fresh local handoff evidence; no commit.

- [ ] **Step 1: Run catalog tests three times**

```bash
for run in 1 2 3; do
  python3 -B tests/task8c_catalog_test.py -q || exit 1
done
```

- [ ] **Step 2: Run adjacent regressions**

```bash
python3 -B tests/task8c_artifacts_test.py -q
python3 -B tests/task8c_profiles_test.py -q
python3 -B tests/task8c_rate_point_test.py -q
python3 -B tests/generate_source_manifest_test.py -q
```

- [ ] **Step 3: Run registered CTest**

Configure fresh `build-catalog-local` with `BUILD_TESTING=ON`,
`BUILD_RDMA_PIPELINE=OFF`, `USE_CUDA=OFF`, then run:

```bash
ctest --test-dir build-catalog-local \
  -R '^(task8c_catalog_test|task8c_artifacts_test|task8c_profiles_test|task8c_rate_point_test|generate_source_manifest_test)$' \
  --output-on-failure
```

- [ ] **Step 4: Verify boundaries and review**

Create an ignored temporary `test-results` fixture and exercise
import/rebuild/query/verify. Confirm `git status --short` excludes runtime
evidence and tests leave `accepted-results.json` unchanged. Run
`git diff --check`. Review path containment, atomic replacement, symlink/
traversal rejection, deterministic ordering, Python 3.8 compatibility, and
absence of network/Git subprocesses.

---

### Task 8: Server Acceptance and Real Cross-Task Handoff

**Files:**
- Server-test handoff only; testing task modifies no product code
- Local runtime output: `test-results/suites/<suite-id>` plus generated catalogs

**Interfaces:**
- Consumes: source manifest and the next real compact remote suite.
- Produces: one verified local catalog entry and successful callbacks to both tasks.

- [ ] **Step 1: Run Phase 0**

Verify HF/qths/qtp connectivity, exact Python/transfer tools, local destination
permissions, and absence of conflicting resources. Missing facilities return
`ENV_BLOCKED`; do not invent copy commands.

- [ ] **Step 2: Synchronize and run regressions**

Mirror the complete tree, verify manifest, and run direct catalog tests three
times plus registered CTest three times on qths1. These tests create no
rings/network/GPU resources.

- [ ] **Step 3: Validate a compact fake suite**

Transfer one task-owned fake compact suite through the observed HF path, import
locally, rebuild/verify/query it, then remove only that fake fixture. Confirm
the tracked accepted-results file is unchanged.

- [ ] **Step 4: Import the next real suite**

After its normal product test and cleanup, copy only the compact suite, verify
the remote manifest, import locally, rebuild/verify, and query by exact suite
ID. Confirm local summary/preflight/runs and evidence SHA agree with the remote
result report.

- [ ] **Step 5: Send the development callback**

Send the complete report to source task
`019fbaf1-006f-7970-8566-5d3d51086698`. Verify the tool response identifies
the intended task; retry one failed delivery once and report notification
failure independently if retry also fails.

- [ ] **Step 6: Verify cleanup and return status**

Require no task-owned process/ring/capability or temporary import directory.
Return `TEST_RESULT`, `CLEANUP_RESULT`, `RESULT_NOTIFICATION`,
`REMOTE_SUITE_PATH` and `SUITE_MANIFEST_SHA256`. If all requested gates pass,
remind the user that changes are ready for commit; do not commit or push.
