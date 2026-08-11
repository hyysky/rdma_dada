# Remote Acceptance Preflight Design

## Purpose

Prevent remote acceptance runs from discovering stale binaries, obsolete
headers, incompatible JSON files, command-quoting defects or unavailable sender
endpoints only after receiver resources have started.

The formal test boundary is explicit: no PSRDADA ring, capability, receiver,
worker or sender may be created until the exact rendered inputs pass a remote
preflight using the exact Release build selected for the run.

## Retained Failure Lessons

The preflight is designed to prevent recurrence of these observed harness
failures:

- qths1 was incorrectly used to SSH back to sender hosts instead of HF
  controlling all three hosts independently;
- isolated Codex task worktrees were treated as a shared source tree;
- representative file hashes were mistaken for a complete synchronized build
  identity;
- an implicit `build-linux` directory selected stale binaries instead of the
  verified Release build;
- fixed UDP source ports collided with another socket;
- multi-line `python -c` content was corrupted by the remote SSH shell;
- an obsolete static DADA header omitted the strict ATFP timeline fields;
- individually valid config files were never checked as one resolved runtime
  configuration;
- missing failure-time diagnostics incorrectly contaminated resource cleanup
  status;
- a supposed fail-closed gate used an old harness and created remote resources.

Each lesson must have an automated regression test or an explicit preflight
acceptance check. Chat history is not an authoritative test specification.

## Components

### `vdif_unpack_preflight`

A portable C++ executable links the same production libraries used by
`rdma2dada` and `vdif_unpack_worker`. It accepts four explicit paths:

1. pipeline JSON;
2. packet-format JSON;
3. worker JSON;
4. rendered raw DADA header.

It performs the following operations using production parsers and validators:

- load the worker, pipeline and packet-format configurations;
- compute raw record, raw block, compute block and ring geometry;
- parse the actual 4096-byte DADA header artifact;
- validate RAW/TFP/Project-VDIF metadata against the resolved configuration;
- parse the strict VDIF observation timeline;
- validate `GROUP_PERIOD_PS`, group start epoch/seconds/frame and
  `EXPECTED_GROUPS`;
- build the expected UNPACKED/ATFP output header;
- verify `LAYOUT_SCOPE=BLOCK`, exact output block bytes and transfer bytes;
- reject unknown, missing, obsolete or conflicting fields.

On success it prints one machine-readable JSON object containing the resolved
geometry, timeline, Station count, input/output order and stop boundary. On
failure it exits nonzero with the first production-parser error.

The tool never connects to PSRDADA and never opens a network device.

### Per-run raw DADA header

The controller renders a complete header inside the run bundle. It no longer
references a server-resident static template.

For the current fixture it derives:

- `GROUP_PERIOD_PS = PKT_NSAMP * sample_interval_ps`;
- `GROUP_START_REFERENCE_EPOCH`, `GROUP_START_SECONDS` and
  `GROUP_START_FRAME` from the exact sender observation start;
- `EXPECTED_GROUPS = RatePlan.group_count`;
- raw record, block, rate and transfer fields from the same pipeline geometry.

The rendered header is transferred and SHA-verified like every JSON input.
`rdma2dada --dump-header` receives this scoped file path.

### Controller `--preflight-only`

The existing versioned controller gains a real preflight entry path. It:

1. verifies HF/qths1/qtp tool and file freshness required by the run;
2. requires an explicit qths1 Release binary directory;
3. renders all final configs, header and helper scripts;
4. creates only scoped temporary directories;
5. transfers every artifact and verifies SHA256;
6. runs `vdif_unpack_preflight` from the selected Release directory on qths1;
7. transfers and executes the SHA-verified sender endpoint probe files;
8. records binary paths/SHA, config SHA, endpoint ports and validator JSON;
9. removes scoped temporary directories;
10. exits without creating rings, changing capabilities or starting processes.

The command produces `manifest.json`, `state.json` and `result.json` with
`TEST_RESULT=PASS` only when all checks complete. Cleanup state remains
independent.

`--preflight-only` and `--execute` are mutually exclusive. Every `--execute`
run repeats the same preflight inline with its own final artifacts before
creating resources; it does not trust a result from an earlier run. The
standalone mode is the required acceptance gate for the controller path, while
the inline repetition closes the time-of-check/config-regeneration gap. A
dry-run alone is never sufficient.

## Formal Test Sequence

The Task 5 correctness acceptance sequence is:

1. synchronize the complete declared manifest through the explicit handoff;
2. build the validator and Task 5 binaries in one Release directory;
3. run portable/controller tests three consecutive times;
4. run `--preflight-only` using the exact formal rate plan;
5. confirm no process, ring or capability was created by preflight;
6. run three independent 0.1 Gbps dbdisk measured transfers;
7. stop on the first failed Station, harness stage, product check or cleanup;
8. do not proceed to higher rates or Task 6 until all three runs pass.

Each formal run must verify:

- both Station senders scheduled and sent every group;
- sender sent total equals receiver accepted, receiver published and unpack
  records;
- CQ tail and partial raw block accounting conserve every complete record;
- no bad header, invalid data, unknown Station, duplicate, late, out-of-range,
  missing Station or fully missing group occurs in the all-valid fixture;
- compute output is `UNPACKED/ATFP`, block-scoped, byte-exact and has the known
  sample oracle;
- DADA header, data and EOD are complete;
- cleanup removes only the run-owned processes, rings, capabilities and
  temporary paths.

## Failure Classification

- Source/config transfer, SHA mismatch, validator invocation or result parsing:
  `HARNESS_FAIL`.
- Missing/incompatible required executable, permission, NIC, source address or
  occupied sender endpoint: `ENV_BLOCKED`.
- A successfully started product violates header, counter, data, EOD or
  throughput contracts: `PRODUCT_FAIL`.
- Missing optional diagnostics are recorded under `diagnostic_errors` and do
  not alter `CLEANUP_RESULT`.
- Actual process, ring, capability or scoped-directory cleanup failures set
  `CLEANUP_RESULT=FAIL` without replacing the original test outcome.

## Acceptance of the Preflight Feature

Before it controls a formal server run, automated tests must demonstrate:

- valid rendered inputs produce the expected validator JSON;
- each required timeline field missing or conflicting is rejected;
- obsolete packet fields and stale binary selection are rejected;
- `--preflight-only` creates no runtime resources;
- SHA mismatch, validator failure and endpoint failure retain the first error;
- cleanup and diagnostic collection remain independently classified;
- the real qths1 Release validator passes the exact transferred artifacts three
  consecutive times.
