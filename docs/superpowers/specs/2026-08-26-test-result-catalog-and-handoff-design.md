# Test Result Catalog and Cross-Task Handoff Design

## Purpose

Provide one reproducible, selectively readable interface between remote server
testing, module development and paper/report writing. The catalog must answer
which test ran, with which exact configuration and identity, what result it
produced, where its compact evidence lives and whether cleanup succeeded.

The catalog is an index, not a replacement for suite evidence. Per-suite JSON
remains authoritative. No result may be reconstructed from chat text or entered
manually into the catalog.

## Scope

The first version covers the four accepted test topologies:

- `receive`: `rdma2dada -> raw ring -> dada_dbnull`;
- `unpack`: `rdma2dada -> raw ring -> vdif_unpack_worker -> compute ring -> dada_dbnull`;
- `gpu`: `dada_junkdb -> compute ring -> pipeline_worker -> output ring -> dada_dbnull`;
- `full`: `rdma2dada -> raw ring -> vdif_unpack_worker -> compute ring -> pipeline_worker -> output ring -> dada_dbnull`.

It does not store raw data files, build trees, full progress logs or manually
created CSV measurements. It does not change product, controller or test
acceptance semantics.

## Directory Contract

The local development workspace contains an ignored runtime tree:

```text
test-results/
├── suites/
│   └── <suite-id>/
│       ├── summary.json
│       ├── preflight.json
│       ├── runs/
│       │   ├── warmup-01.json
│       │   ├── warmup-01.evidence.log
│       │   ├── measured-01.json
│       │   └── measured-01.evidence.log
│       ├── MANIFEST.sha256
│       └── origin.json
├── catalog.json
├── catalog.csv
└── MANIFEST.sha256
```

`test-results/` is ignored by Git. Each suite directory is immutable after a
successful import. Re-importing identical bytes is idempotent; importing the
same suite ID with different content fails closed.

`origin.json` is generated during import and contains:

```json
{
  "schema_version": 1,
  "suite_id": "unpack-30Gbps-60s-20260826T120000Z",
  "source_host": "qths1",
  "remote_suite_root": "/home/user/wy/task8c-results/unpack-30Gbps-60s-20260826T120000Z",
  "imported_utc": "2026-08-26T12:30:00Z",
  "source_manifest_sha256": "<64 lowercase hex characters>"
}
```

The importer first copies into a temporary sibling directory, verifies the
remote suite manifest, validates required files and JSON schemas, writes
`origin.json`, then atomically renames the directory into `suites/<suite-id>`.
An interrupted import must not create a catalog entry.

## Authoritative and Derived Data

Authority order is:

1. `suites/<suite-id>/summary.json` for suite outcome and repetitions;
2. `suites/<suite-id>/preflight.json` for resolved plan, environment and
   configuration identity;
3. `suites/<suite-id>/runs/*.json` for per-run process, metric, failure and
   cleanup evidence;
4. `suites/<suite-id>/runs/*.evidence.log` only for raw-line audit;
5. `catalog.json` and `catalog.csv` as rebuildable indexes.

The index builder scans only valid immutable suite directories. It never scans
chat history or remote server logs and never infers missing values.

## Catalog JSON Schema

`catalog.json` is one object rather than append-only JSONL so it can be written
atomically and verified as a complete snapshot:

```json
{
  "schema_version": 1,
  "generated_utc": "2026-08-26T12:31:00Z",
  "suite_count": 1,
  "suites": [
    {
      "suite_id": "unpack-30Gbps-60s-20260826T120000Z",
      "test_topology": "unpack",
      "modules": ["rdma2dada", "vdif_unpack_worker", "dada_dbnull"],
      "started_utc": "2026-08-26T12:00:00Z",
      "target_payload_gbps": 30.0,
      "actual_payload_gbps": {
        "median": 29.9999,
        "minimum": 29.9998,
        "maximum": 30.0000
      },
      "duration_seconds": 60.0,
      "warmup_count": 1,
      "measured_count": 3,
      "test_result": "PASS",
      "cleanup_result": "PASS",
      "first_failure": null,
      "identity": {
        "source_manifest_sha256": "<64 lowercase hex characters>",
        "config_id": "<64 lowercase hex characters>",
        "geometry_id": "<64 lowercase hex characters>",
        "baseline_profile_id": "qths1-unpack-30gbps-v1",
        "baseline_profile_sha256": "<64 lowercase hex characters>",
        "binary_sha256": {
          "qths1:rdma2dada": "<64 lowercase hex characters>",
          "qths1:vdif_unpack_worker": "<64 lowercase hex characters>"
        }
      },
      "configuration": {
        "receiver_poll_cpu": 13,
        "worker_cpu_list": "14,15,16,17,18,19",
        "gpu_worker_cpu": null,
        "sink_cpu_list": "20",
        "numa_node": 1,
        "receiver_poll_batch": 32,
        "receiver_wr_num": 1024,
        "rings": {
          "raw": {"block_bytes": 52838400, "blocks": 16},
          "compute": {"block_bytes": 52428800, "blocks": 8}
        },
        "window_groups": 198400,
        "reorder_horizon_groups": 96000,
        "unpack_start_delay_seconds": 1
      },
      "result_path": "suites/unpack-30Gbps-60s-20260826T120000Z",
      "summary_sha256": "<64 lowercase hex characters>",
      "suite_manifest_sha256": "<64 lowercase hex characters>"
    }
  ]
}
```

Fields not used by a topology are JSON `null` or omitted only where the schema
explicitly permits omission. Unknown result states or missing required identity
fields make the suite invalid for the catalog; they are not converted to empty
strings or zero.

`first_failure` is either `null` or an object containing `run_id`, `stage`,
`classification`, `exit_code` and a short evidence-backed message. Full stdout
and stderr remain in the failed suite's compact debug artifact.

## CSV View

`catalog.csv` is generated from the same in-memory entries as `catalog.json`.
It contains one row per suite and a stable flattened subset:

```text
suite_id,started_utc,test_topology,modules,target_payload_gbps,
actual_payload_gbps_median,duration_seconds,warmup_count,measured_count,
test_result,cleanup_result,first_failure_stage,first_failure_classification,
baseline_profile_id,source_manifest_sha256,config_id,geometry_id,
receiver_poll_cpu,worker_cpu_list,gpu_worker_cpu,sink_cpu_list,numa_node,
receiver_poll_batch,receiver_wr_num,result_path
```

Nested binary and ring identities remain in JSON. CSV is for human review and
must never be edited as an input.

## Versioned Tool Interface

Create `scripts/task8c_catalog.py` with five subcommands:

```bash
python3 scripts/task8c_catalog.py import \
  --results-root test-results \
  --source-dir /private/tmp/unpack-30Gbps-60s-20260826T120000Z \
  --source-host qths1 \
  --remote-suite-root /home/user/wy/task8c-results/unpack-30Gbps-60s-20260826T120000Z \
  --source-manifest-sha256 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa

python3 scripts/task8c_catalog.py rebuild \
  --results-root test-results

python3 scripts/task8c_catalog.py query \
  --results-root test-results \
  --topology unpack \
  --rate-gbps 30 \
  --result PASS \
  --latest 1 \
  --format json

python3 scripts/task8c_catalog.py verify \
  --results-root test-results

python3 scripts/task8c_catalog.py promote \
  --results-root test-results \
  --suite-id unpack-30Gbps-60s-20260826T120000Z \
  --accepted-output docs/results/accepted-results.json
```

`import` validates the copied source directory and its manifest, derives the
suite ID from the validated summary, writes `origin.json` from explicit source
arguments including the development source-manifest SHA used for the remote
mirror, and atomically installs an immutable suite. It then rebuilds and
verifies the catalog. It never invokes SSH or deletes the caller's source
directory.

`rebuild` validates every suite, sorts entries by `started_utc` then
`suite_id`, writes JSON/CSV to temporary files, atomically replaces both and
regenerates the root manifest. A single invalid suite fails the rebuild and
leaves the previous catalog unchanged.

`query` supports exact `suite-id`, topology, target rate, result, cleanup
result, date range, baseline profile ID and latest count. `--format json`
returns complete selected catalog entries; `--format paths` returns only local
suite paths for an agent that will read authoritative JSON next.

`verify` checks the root manifest, each suite manifest, every indexed summary
SHA, catalog ordering, suite count and absence of unindexed immutable suites.

`promote` requires an existing verified suite ID and copies only its catalog
entry into the tracked accepted-results JSON. It writes atomically, rejects a
conflicting duplicate and performs no Git command.

Remote artifact transfer remains orchestrator-owned. The versioned importer
receives an already copied temporary suite directory; it does not contain SSH
credentials or invent remote shell commands.

## Server-Test Handoff

After every remote test suite, including FAIL, BLOCKED or INCOMPLETE:

1. `GPU服务器代码测试` completes scoped remote cleanup independently of the
   product result.
2. It compacts the suite using the existing result contract.
3. It retains the exact compact-suite path and manifest SHA256.
4. It sends the complete callback to the originating development task:

```text
RESULT_NOTIFICATION=<PASS|FAIL>
SUITE_ID=<suite-id>
TEST_TOPOLOGY=<receive|unpack|gpu|full>
TEST_RESULT=<result>
CLEANUP_RESULT=<result>
REMOTE_SUITE_PATH=<absolute compact-suite path>
SUITE_MANIFEST_SHA256=<sha256>
```

The development task verifies/transfers the compact suite, imports it, rebuilds
and verifies the catalog, and records `CATALOG_IMPORT_RESULT` independently of
the remote test and cleanup results. It alone maintains the test summary table
and user-approved accepted-results index.

The summary task receives no per-test notification. It uses `query` against the
development-maintained catalog only when the user requests a chapter, table,
comparison or status update.

## Selective Reading Workflow

For a request such as “summarize accepted 30 Gbps unpack tests,” the summary
task runs:

```bash
python3 scripts/task8c_catalog.py query \
  --results-root test-results \
  --topology unpack --rate-gbps 30 --result PASS \
  --format paths
```

For each returned path it reads only:

- `summary.json`;
- `preflight.json`;
- the referenced `runs/*.json`.

It reads `*.evidence.log` only when auditing a raw claim or diagnosing a
failure. It does not scan unrelated suites, full historical logs or chat
messages.

## Git and Publication Boundary

Add `/test-results/` to `.gitignore`. Runtime catalogs and complete compact
suites are not committed.

Formal results selected for papers or permanent project status may be promoted
only after explicit user approval into:

```text
docs/results/accepted-results.json
```

This tracked file contains selected catalog entries plus their exact external
suite paths and SHA256 identities; it does not embed evidence logs. Promotion
is an explicit versioned command and never occurs automatically after PASS.
Git commit and push remain user decisions.

## Validation

Unit tests in `tests/task8c_catalog_test.py` cover:

- valid PASS and failed suite indexing;
- all four topologies;
- exact identity/configuration extraction;
- deterministic JSON and CSV ordering;
- selective query filters and latest selection;
- invalid JSON, missing required artifacts and manifest mismatch;
- idempotent identical import and conflicting duplicate suite rejection;
- interrupted temporary import exclusion;
- atomic rebuild preserving the previous catalog on failure;
- independent test, cleanup, import and notification statuses;
- explicit accepted-result promotion without automatic Git operations.

Local acceptance requires the catalog tests and existing artifact/controller
tests to pass three consecutive times, source manifest verification and
`git diff --check`. Server acceptance uses one compact fake fixture first, then
the next real remote suite. It must prove local import, catalog rebuild/query,
both task notifications and zero remote/local runtime-resource residue.
