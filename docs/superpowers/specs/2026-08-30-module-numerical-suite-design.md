# Module Numerical Suite Design

## Purpose

Provide one versioned, machine-readable correctness suite for the numerical
modules used by the paper. The suite turns existing CPU and CUDA unit-test
calculations into three consecutive, traceable repetitions without changing
product algorithms or the throughput-test contract.

The reporting task must be able to discover an accepted suite from a small
tracked index and support every numerical claim by reading only the suite
root's `summary.json`, `preflight.json`, `runs/*.json`, and immutable
`runs/*.evidence.log` files.

## Scope

The suite covers these module/backend pairs:

- complex conversion/transpose: CPU reference and CUDA;
- beamforming: CPU reference and CUDA;
- Power: CPU reference and CUDA;
- coherency products: CPU reference and CUDA, with products
  `AA`, `BB`, `AB_REAL`, and `AB_IMAG`;
- time integration: CPU reference and CUDA, covering both `SUM` and `MEAN`.

The coherency reference also records the numerical check for derived
`I`, `Q`, `U`, and `V`. These remain reference-derived values, not additional
product output arrays.

The change does not modify module implementations, `pipeline_worker`, RDMA,
unpack, PSRDADA rings, the Full runner, or existing throughput suites.

## Evidence Contract

Each numerical test executable accepts an optional
`--result-json <absolute-path>` argument. Its existing no-argument CTest
behavior remains unchanged. Evidence schema version 1 contains:

- `module` and `backend` (`CPU_REFERENCE` or `CUDA`);
- one or more named numerical cases;
- input and output order, sample format, shape, and byte count;
- product labels when applicable;
- integration operation and length when applicable;
- absolute and relative tolerances;
- measured maximum absolute and relative errors;
- NaN and infinity counts;
- overall `test_result`.

Each case must calculate its error fields from the values produced by the
real module under test and an independent expected array already present in
the test. A process exit code of zero without complete evidence is a harness
failure, not a numerical PASS.

Common test-only helpers provide deterministic JSON serialization and error
measurement. Product libraries must not depend on those helpers.

## Versioned Suite Runner

`scripts/module_numerical_suite.py` owns the complete lifecycle. Its accepted
entry command receives the build directory, result root, source manifest, and
requested repetition count. Formal acceptance fixes the repetition count at
three.

Before execution it verifies:

- the complete source SHA256 manifest;
- every required executable path and SHA256;
- host, operating system, CUDA runtime, NVIDIA driver, and GPU identity;
- that CUDA tests can address the selected device;
- that the destination suite does not already exist.

It then runs all ten module/backend tests once per repetition. A failed test,
missing JSON file, invalid schema, tolerance violation, or nonzero NaN/Inf
count fails that repetition and stops before later repetitions. The runner
does not retry or select the best repetition.

## Compact Artifact Layout

```text
module-numerical-<UTC>/
├── preflight.json
├── summary.json
├── MANIFEST.sha256
└── runs/
    ├── measured-01.json
    ├── measured-01.evidence.log
    ├── measured-02.json
    ├── measured-02.evidence.log
    ├── measured-03.json
    └── measured-03.evidence.log
```

`preflight.json` records environment and executable identities.
Each run JSON embeds the ten validated evidence objects and the exact argv,
exit status, start/end timestamps, and evidence-log SHA256. The evidence log
contains one bounded canonical line per module/backend result and is not a
copy of complete CTest output.

`summary.json` records requested/completed repetitions, every run outcome,
the source-manifest SHA256, suite result, and cleanup result. Numerical tests
do not create runtime resources, so successful cleanup means all task-owned
temporary evidence files were compacted or removed.

`MANIFEST.sha256` covers every retained compact file and no unreferenced
artifact is permitted.

## Discovery and Query

Module evidence is intentionally separate from rate-point results because it
has no rate, duration, ring, sender, or topology semantics.

The local catalog lives at:

```text
test-results/module-suites/<suite-id>/
```

The tracked discovery index is:

```text
docs/results/accepted-module-results.json
```

Only a validated three-repetition PASS may be promoted. Each index entry
records suite ID, result path, source and executable identities, repetition
count, module/backend coverage, summary SHA256, suite-manifest SHA256, and
promotion timestamp.

The runner supplies `validate`, `import`, `query`, and `promote` operations.
Import and promotion are idempotent and reject conflicting suite identities.
The reporting task first reads the tracked index, then reads only the compact
suite JSON files at the indexed local path. It never scans build logs or
reconstructs evidence from prose.

## Testing and Acceptance

Local tests cover evidence-schema rejection, incomplete module coverage,
tolerance/NaN/Inf failure, identity mismatch, interrupted repetition,
manifest validation, idempotent import, query, and promotion.

GPU-server acceptance uses one fresh CUDA Release build and one formal suite
with three consecutive repetitions. It does not create rings or run network,
RDMA, unpack, GPU pressure, or Full pipeline data flow.

Acceptance requires:

- every CPU and CUDA evidence object is complete and within tolerance;
- all three repetitions PASS without retry;
- source, executable, CUDA, driver, and GPU identities are retained;
- compact MANIFEST verification passes locally and on the test host;
- the suite imports and queries successfully;
- promotion updates `accepted-module-results.json` through the versioned tool;
- the reporting task can resolve the accepted suite and its numerical fields
  without reading any full log.
