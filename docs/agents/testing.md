# Reproducible Test Policy

This document defines how a test becomes repeatable acceptance evidence for
`rdma_dada`. It applies to portable unit tests, CUDA modules, PSRDADA rings,
RDMA/UDP ingest, unpack, pipeline integration and throughput measurements.

## Acceptance is a repeatable procedure

A test is not complete because it passed once. Final acceptance requires a
versioned runner that can recreate the test from a clean state without relying
on remembered commands, interactive shell state or manually edited remote
files.

The runner must provide one documented entry command and accept all variable
inputs through explicit CLI arguments or a recorded configuration file. It
must create a unique run ID, persist its state, produce a machine-readable
result and clean only resources owned by that run.

Finite network fixtures must preserve the requested observation stop boundary;
they must not round `group_count` to a sender batch or raw-ring block. On
orderly stop, every accepted complete record is published, including CQ entries
below the configured poll batch and the final partial raw block. Acceptance requires
`sender sent = receiver accepted = receiver published = unpack records`; a
partial final block is normal finite-transfer behavior and must not be counted
as packet loss or a throughput failure.

## Phase 0: target-server environment preflight

Before designing or changing a remote test controller, fixture or script, run
a read-only preflight on every target host and return its evidence to the
development task. The test design must use the environment that was actually
observed, not assumptions copied from macOS, another server or a previous run.

At minimum, record:

- exact paths, versions and supported CLI options for every required binary;
- dynamic-library resolution and relevant runtime/toolchain versions;
- required user, device, network and capability permissions;
- NIC device/link state, addresses, MTU, queue information and NUMA placement;
- available CPU, memory and disk capacity plus the intended output location;
- UTC/NTP/PTP state and command/shell behavior that affects argument parsing;
- SHA256 values for the tested executables and supplied source/config files.

If any required check cannot be completed, or a required facility is missing
or incompatible, stop before harness implementation or formal execution and
report `TEST_RESULT=ENV_BLOCKED` with the exact command and output. Propose the
smallest environment decision needed; do not guess a path, silently install a
dependency or hide the blocker in a test-script workaround. Repeat Phase 0
after any approved environment change.

### Minimum repetition

| Test class | Final acceptance requirement |
| --- | --- |
| Unit and portable component | Three consecutive clean executions of the relevant test command |
| GPU numerical module | Three consecutive executions using the same recorded input/seed and tolerance, with every comparison passing |
| PSRDADA/RDMA integration | Three fresh ring/process lifecycles with reconciled producer, receiver, unpack and consumer counts |
| Functional multi-Station network | Three complete synchronized transfers with correct Station mapping, headers, TFPA data and EOD |
| Performance rate point | One warm-up plus at least three measured runs; every measured run must satisfy correctness and stability gates |
| Fault injection | A recorded seed and manifest, repeated with identical injected records and identical expected counters |

Additional repetitions may be required for timing-sensitive or intermittent
failures. Reports include every run; do not discard failed runs or select only
the best result.

## Required result artifacts

The controller exposes exactly four formal boundaries:

| Stage | Data path |
| --- | --- |
| `receive` | UDP senders → `rdma2dada` → raw ring consumer |
| `unpack` | UDP senders → `rdma2dada` → raw ring → `vdif_unpack_worker` → compute ring consumer |
| `gpu` | block producer → compute ring → `pipeline_worker` → output ring consumer |
| `full` | UDP senders → `rdma2dada` → unpack → compute ring → GPU worker → output ring consumer |

`unpack` is intentionally not a synthetic stand-alone input mode. GPU-only
creates no raw ring, network receiver or sender and requires no `CAP_NET_RAW`.
The configurable target always means aggregate payload rate at the selected
boundary.

The GPU boundary uses the repository-owned `gpu_pressure_writer`. It publishes
the compiler-generated `unpacked.header` unchanged, paces complete compute
blocks from one monotonic epoch, and records exact writer block/byte/EOD and
wait/lateness metrics. Stock `dada_junkdb` is not an accepted formal producer.
For any positive configured target rate, not only 30 Gbps, the block plan is:

```text
target_bytes_per_second = target_payload_bits_per_second / 8
blocks_per_second = ceil(target_bytes_per_second / compute_block_bytes)
actual_bytes_per_second = blocks_per_second * compute_block_bytes
total_blocks = blocks_per_second * duration_seconds
```

The result records target rate, block-aligned configured injection rate,
writer-measured publication rate (`published_bytes / active_elapsed_ns`), and
worker-measured active processing rate separately. Suite summary records
`actual_aggregate_gbps_source=input_writer.actual_payload_gbps`; the configured
block-aligned rate is never presented as measured throughput. PASS requires the writer
rate to remain within 2% of the block-aligned plan and the finite transfer to
finish within 102% of the planned duration; eventual count closure after
backpressure is not a performance PASS. Because of upward rounding, the
configured injection rate is never lower than the target. This tests
sustained block pressure and backpressure. The same product fixture can select
`SYNCHRONOUS_DIRECT/1` or `STAGED_PIPELINE/N`; a matched comparison changes
only those CUDA execution fields. `full` remains the final astronomical
data-flow acceptance and must not start `gpu_pressure_writer`.

For `SYNCHRONOUS_DIRECT/1`, `blocks`, `input_bytes`, `output_bytes`, transfer
elapsed time and CUDA stage totals are the authoritative worker closure and
timing fields. The scheduler lifecycle fields `submitted_blocks`,
`completed_blocks`, `published_blocks`, `max_inflight` and active-window fields
belong to `STAGED_PIPELINE/N` and remain zero in the direct reference path;
they must not be interpreted as zero processed blocks. The compact plan also
retains the compiler's `gpu_pipeline_budget`, so memory/deadline parameters do
not depend on the direct runtime metrics object's staged-only fields.

The production-pressure geometry is `A=469,F=4,P=1,B=350`, CI8 complex, with
every `F` representing exactly 1 MHz. Its astronomical signal payload is
30.016 Gbps. The wire fixture uses 512 samples per packet (4096-byte payload)
and 26 groups per block. Matching ATFP geometry and FPAB weights make the CUDA
conversion, GEMM shape, output expansion, transfers and device buffers
representative. The production Power fixture uses `STAGED_PIPELINE/3`, Power
followed by 128-sample MEAN integration, and a 582,400-byte output block. The
versioned runner continues to accept its existing aggregate VDIF-record rate;
the nominal 30-Gbps point may follow the exact geometry-derived rate rather
than being forced to exactly 30.000 Gbps.

For a `full` network test, the controller partitions the exact compiler-resolved
Station list into 235 IDs on qtpulsar1 and 234 on qtpulsar2. Each host runs one
schema-v3 `fpga_sender_sim`, one fixed source endpoint and one socket. Their
signal payloads are 15.040 and 14.976 Gbps; because every UDP datagram also
contains the 32-byte VDIF header, the sender `target_gbps` values are 15.1575
and 15.093 Gbps, for 30.2505 Gbps total VDIF-record traffic. Forcing an equal
15/15 split would violate the fixed 1 MHz-per-channel observation geometry.
Sender result JSON must close aggregate and per-Station packet counts before
receiver/unpack/GPU results can be accepted.

The complete Station list is a Resolved Plan property, not a ring-header
field. The sender uses it for host sharding and unpack uses it to map VDIF
Station IDs into the fixed ATFP `A` axis. Ring headers retain `NANT`,
`CONFIG_ID`, and `GEOMETRY_ID`, but omit `STATION_IDS`; downstream processing
therefore consumes the already ordered complete array without duplicating an
O(A) list inside each fixed 4096-byte header.

### Paper acceptance protocol

Paper-facing Section 3/4 acceptance uses physical untagged-IPv4 Ethernet line
rate as its primary rate definition and the production geometry
`A=469,F=4,P=1,B=350`. Every compact result must also retain the corresponding
UDP datagram/payload rate and astronomical signal payload rate so the three
definitions can be reconciled without reconstructing them from prose. The two
physical sender hosts shard the exact Station list 235/234; an `A=2` fixture is
only a functional or prototype geometry and cannot establish the paper's final
production-scale claim.

Power and coherency are separate formal product modes. The coherency output
contract remains `AA`, `BB`, `AB_REAL`, `AB_IMAG`. A numerical reference may
also verify `I=AA+BB`, `Q=AA-BB`, `U=2*AB_REAL`, and `V=-2*AB_IMAG`, but those
derived values are not separately published arrays unless the product contract
is changed explicitly.

The initial production coherency geometry is `A=469,F=2,P=2,B=350`, with each
`F` still representing 1 MHz. Doubling polarization while halving channel count
keeps the astronomical signal payload near the Power profile's 30-Gbps target;
the compiler and sender plan must derive and record the exact physical, UDP and
signal rates rather than copying a nominal value. Power retains
`A=469,F=4,P=1,B=350`.

For Project Observation v1, `NPOL=2` has the fixed polarization order `X,Y`.
The compiler writes `POL_LABELS X,Y` into the stage-header chain automatically;
the Observation JSON does not carry a separately configurable label list.

The paper's primary gate is the nominal 30-Gbps campaign for 60 seconds with
one warm-up and three measured repetitions. Report the measured or uniformly
derived physical Ethernet line rate as the result value while retaining UDP
record and astronomical signal rates in the compact artifacts. The historical
30-second `1/5/10/20/30/35/40` ladder remains an optional saturation campaign,
not a prerequisite for the accepted primary point.

Production Full Power suite `full-30.2505Gbps-60s-20260829T033639Z` and Full
coherency/Stokes suite `full-30.2505Gbps-60s-20260829T075447Z` satisfy this
primary repetition protocol. Reuse their source ports, one-second preparation,
CPU/NUMA placement, queue and ring geometry for comparable work. Rerun an
unchanged upstream boundary only when a product change or a named experiment
affects it.

Section 3 requires a receive-only matched placement experiment between staged
payload copy and `NSGE=2` direct raw-ring placement. Its boundary is
senders → `rdma2dada` → raw ring → `dada_dbnull`; unpack and GPU are absent.
Change only placement mode: receiver/NIC/link/MTU, Project VDIF record bytes,
physical-line-rate packet rate, sender/flow count, endpoint/flow-rule structure,
pacing/batching, aggregate physical line rate, duration, warm-up/measured count,
raw ring, WR/poll settings, CPU affinity, NUMA, consumer and drain policy stay
identical. `A=469` is preferred for workload consistency but is not a causal
matching requirement at this boundary; another Station identity is valid when
record size, packet rate and flow topology match exactly. The compact result must
record placement mode, measured physical line rate, repetitions, packet
loss/deficit, receive-to-publication service P50/P95, receiver CPU-seconds/GB,
raw-ring HWM/backpressure, copy bytes/memory traffic and minimum headroom. If
copy fails while direct passes, retain the effective failure boundary and first
saturated boundary; sender rate alone cannot establish either result.

This experiment is complete. Direct suite
`rdma2dada-30Gbps-60s-20260829T054121Z` closed all counts; staged-copy suite
`rdma2dada-30Gbps-60s-20260829T055947Z` retained an approximately 0.54%
receiver deficit. Reporting must apply the matching entry in
`docs/results/evidence-adjudications.json`, which preserves the original runner
PASS while exposing the effective `PERFORMANCE_FAIL/COUNT_CLOSURE_FAIL`
decision. Do not rerun this comparison unless placement code or a matched
control parameter changes.

Rate-point result directories use
`<pipeline-stage>-<aggregate-rate>Gbps-<duration>s-<UTC timestamp>` so rate and
time are visible without opening the manifest.

JSON is the machine-readable interface used by reporting tasks. A suite keeps
immutable configuration once and stores one JSON result plus one compact raw
evidence log per repetition:

```text
observation.json
resolved_observation.json
preflight.json
summary.json
MANIFEST.sha256
runs/
  warmup-01.json
  warmup-01.evidence.log
  measured-01.json
  measured-01.evidence.log
  measured-02.json
  measured-02.evidence.log
  measured-03.json
  measured-03.evidence.log
```

Each run JSON is authoritative and includes the effective process ledger,
state timeline, structured counters, result and cleanup. Every process entry
records `host`, `role`, complete `argv`, allowlisted `env`, `cpu_affinity`,
`numa_node`, `thread_mapping`, `binary_sha256`, `config_sha256`, `pid`,
`started_utc`, `ended_utc` and `returncode`. Ring
geometry is part of `resolved_observation.json`; it is not duplicated as a
top-level result file.

Each `*.evidence.log` contains only unmodified final summary, EOD, warning,
error and resource cleanup lines, prefixed with their source process. It is
proof for auditing the structured JSON, not an alternate reporting interface.
The run JSON records its evidence filename and SHA256. `MANIFEST.sha256` covers
every retained suite artifact.

### Local catalog and reporting handoff

Compact remote suites are imported into the local, Git-ignored
`test-results/suites/<suite-id>/` tree with `scripts/task8c_catalog.py`. Import
must verify the remote suite manifest, record `source_host`, remote suite root
and the development source-manifest SHA in `origin.json`, and atomically publish
the immutable suite. `catalog.json` and `catalog.csv` are deterministic derived
views and are never edited as evidence.

Reporting tasks query by suite ID, topology, target rate, result, cleanup
result, date or passing-profile identity. They read only the selected suite's
`summary.json`, `preflight.json` and `runs/*.json`; `*.evidence.log` is opened
only to audit a cited raw line or diagnose a failure. The tracked
`docs/results/accepted-results.json` contains only suites explicitly promoted
with user approval. Promotion records evidence selection, not necessarily a
product PASS.

Remote result roots are retained until the user authorizes deletion. Maintain
an inventory entry for every formal or diagnostic suite containing its purpose,
topology/product mode, geometry, rate definitions, duration/repetitions,
source/config/binary identities, result, cleanup, suite path, manifest SHA and
whether it has been imported into the local catalog. A formal test is not
closed until its compact manifest is verified, the suite is imported, and a
bounded result report with the exact suite path and manifest SHA has been sent
successfully to the originating development task. The paper/reporting task
discovers results through the catalog query interface; direct notification is
optional unless explicitly requested.

After remote cleanup, the testing task sends the complete result and the exact
compact-suite location to the originating development task. The development
task verifies/transfers the suite, runs the local catalog importer, and
maintains the generated catalog and any user-approved accepted-results entry.
The callback is:

```text
RESULT_NOTIFICATION=<PASS|FAIL>
SUITE_ID=<suite-id>
TEST_TOPOLOGY=<receive|unpack|gpu|full>
TEST_RESULT=<result>
CLEANUP_RESULT=<result>
REMOTE_SUITE_PATH=<absolute compact-suite path>
SUITE_MANIFEST_SHA256=<sha256>
```

Remote test result, cleanup result and development `RESULT_NOTIFICATION` are
independent states. After receipt, the development task records
`CATALOG_IMPORT_RESULT` independently; an import failure never rewrites the
remote test or cleanup result. `总结成文` receives no per-test callback. When
asked to update a chapter, table or status, it queries the catalog maintained
by the development task and reads only the selected suite artifacts.

On PASS, remove copied bundles, bootstrap artifacts, helper scripts, PID,
ready/exit files, duplicate headers/configs, raw data and full progress logs
after the JSON and evidence log are durable. On failure add only:

```text
debug/<run-id>/failed-command.json
debug/<run-id>/failed-process.log
debug/<run-id>/resource-snapshot.json
debug/<run-id>/pipeline-worker.log  # GPU/full failure only, when produced
```

The debug files preserve the first failed command, its complete captured
stdout/stderr and relevant cleanup/statistics. GPU/full failures additionally
retain the pipeline worker log so an early transfer-open error is not replaced
by the controller's later statistics-validation symptom. They do not retain
unrelated stages. Preflight belongs to the suite as `preflight.json`; a formal
suite does not create a sibling preflight result directory.

## Versioned passing profiles

The latest accepted host/topology configuration is stored under
`config/testing/profiles/` and is the default comparison baseline for later
tests of that boundary. A profile includes CPU/NUMA placement, coordinator,
parser and writer roles, `gpu_worker_cpu`, sink CPU, queue settings,
ring/window geometry and preparation policy. GPU/full profiles are invalid
without an explicit GPU-worker CPU. Rebuilding binaries updates recorded
binary identities but does not
reset the runtime profile to unpinned or single-thread defaults.

Network baselines also pin the two Station source ports. The controller must
not derive different ports from a new suite name when a passing profile
supplies them. A `full` topology may inherit an accepted `unpack` profile so
receiver CPU/NUMA placement, unpack thread roles, queue geometry, raw/compute
rings, window/reorder geometry, source ports and preparation interval remain
unchanged while GPU/output settings are added. This is profile inheritance,
not a new acceptance run for `rdma2dada + unpack`.

Unless a test is explicitly named as a preparation-policy experiment,
`unpack` and `full` server runs use a one-second unpack preparation interval
(`--unpack-start-delay-seconds 1`). A zero-second interval is a distinct
configuration and its result must not be substituted for the accepted
one-second baseline.

GPU worker performance profiles also pin the Observation
`processing.cuda_pipeline` contract. `SYNCHRONOUS_DIRECT/1` is the retained
unoptimized comparison path. `STAGED_PIPELINE/N` uses exactly `N` bounded
slots (1–4), each with its own non-blocking CUDA stream, pinned output staging
and device buffers. The compute ring is registered once per transfer, and each
input lease remains valid until its H2D event completes. A comparable staged run records execution mode,
slot count, submitted/completed/published block closure, maximum inflight,
completion reorder count, slot/writer wait, ring-registration identity/time,
zero input-staging bytes, CUDA H2D/algorithm/D2H time and planned device/pinned
bytes. The output ring passes
only when the single writer publishes every block in logical sequence;
out-of-order CUDA completion does not permit PSRDADA block reordering.

Split-socket tests record `ingress_numa_node` separately from
`processing_numa_node`. The ingress node owns the raw ring and `rdma2dada`;
the processing node owns `vdif_unpack_worker`, compute/output rings,
`pipeline_worker` and the sink. The legacy `numa_node` field binds both zones
to one node and must not be combined with the split fields.

Every run records the profile path and SHA256. Before resource creation the
controller compares effective parameters with the profile. An exact match
continues as a baseline run. Any difference requires an explicit experiment
name and is written as a field-by-field diff in `preflight.json`; an unnamed
drift is `HARNESS_FAIL`.

Git is managed only in the local development worktree. Remote test hosts do not
need a Git executable and test controllers must not invoke Git there. The
development task supplies source SHA256 values; remote manifests record and
compare those values together with generated configuration and binary SHA256.

The in-progress run result is written before cleanup, augmented with cleanup
status, then compacted into `runs/<run-id>.json`; the test outcome is never
replaced by a final `CLEANED` state. Python controllers
run unbuffered or explicitly flush logs, and unexpected exceptions retain the
traceback and the first failed command's argv, exit code, stdout and stderr.

## Outcome classification

Use distinct fields rather than overloading PASS/FAIL:

- `TEST_RESULT=PASS`: all correctness and acceptance gates passed;
- `TEST_RESULT=PRODUCT_FAIL`: the tested implementation violated its contract;
- `TEST_RESULT=HARNESS_FAIL`: configuration transfer, quoting, controller,
  logging or orchestration failed before a valid product measurement;
- `TEST_RESULT=ENV_BLOCKED`: required hardware, permission, capacity, clock or
  dependency was unavailable after an attempted and recorded check;
- `RESULT_NOTIFICATION=PASS|FAIL`: whether the result callback was delivered;
- `CLEANUP_RESULT=PASS|FAIL`: whether owned resources were removed.

A sender that never started cannot produce a throughput FAIL. A dry-run cannot
produce a functional PASS. A single-Station result cannot be reported as a
multi-Station PASS.

## Controller requirements

Long-running server tests use a versioned controller in this repository. The
controller must:

1. run from one declared control host and avoid recursive control-host SSH;
2. use argument arrays or transferred wrapper files instead of nested shell
   quoting;
3. generate configs deterministically and verify source/target hashes;
4. derive future launch time at execution time and start Station processes
   concurrently;
5. persist state so a Codex turn or SSH client can reconnect without restarting
   the observation;
6. assign unique remote directories, PID files and ring keys or explicitly
   validate exclusive ownership;
7. use `finally`/trap cleanup scoped to recorded resources, never broad `pkill`;
8. retain the original failure after cleanup in the compact run JSON;
9. support `--dry-run` and automated self-tests, including real entry routing,
   early sender failure, expired start-time regeneration, resume and cleanup;
10. fail closed when counts, headers, EOD, logs or result artifacts are missing.

An executable acceptance run must receive the tested qths1 binary directory as
an explicit argument. It must not fall back to a conventional directory such
as `build-linux`, because that directory may contain a valid but stale build.
The manifest records the selected directory and binary hashes before resources
are created.

For the current three-host topology, the declared control host is `HF`.
`HF` must open independent SSH sessions to `qths1`, `qtpulsar1` and
`qtpulsar2`; `qths1` must never be used as a jump host or asked to SSH back to
either sender. Controller-owned `known_hosts` files also live on `HF` unless a
versioned controller explicitly transfers and verifies a scoped copy. A test
that requires receiver-to-sender SSH is not an acceptance entry point for this
topology and must be reported as not applicable rather than changing server
authentication to accommodate the harness.

The controller itself is reviewed and tested like project code. A controller
created only in `/tmp` is diagnostic material and cannot authorize final
acceptance.

For Task 8C startup, the controller exposes one external `PIPELINE_READY`
gate. It creates the rings, starts the compute consumer, starts
`vdif_unpack_worker`, waits for and validates its atomic `worker.ready`, then
starts `rdma2dada` and waits for receiver readiness. Only then does it write
`pipeline.ready` and allow both Station senders to start. `worker.ready`
records the process identity, CONFIG_ID/GEOMETRY_ID, prepared window geometry
and preparation timing. For unpack runs, `--worker-cpu-list` is an ordered,
explicit mapping `COORDINATOR,WORKER...,WRITER`; the worker binds those threads
itself while `numactl --membind` keeps their memory on the selected NUMA node.
Omission preserves normal scheduling. The rendered argv and observed affinity
are retained with the run artifacts.

## Environment and synchronization lessons

- Compare a shared Git commit and source/config manifest. Independently built
  binaries are not required to have identical SHA256 because compiler paths,
  toolchains and build IDs may differ; copy one artifact when byte identity is
  required.
- Resolve server-side tools during Phase 0 and generate remote scripts with
  their verified absolute paths. Non-login SSH does not inherit interactive
  shell PATH additions; a successful interactive command is not evidence that
  the same bare command name will resolve inside the acceptance runner.
- Redirect the file descriptors of an entire background subshell, not only the
  command nested inside it. A background child that retains the SSH channel's
  stdout or stderr keeps the remote SSH call open and can deadlock startup when
  later pipeline stages are responsible for producing its input or EOD.
- Verify host keys with a dedicated temporary `known_hosts`, then use strict
  checking. Create and verify every destination directory before transfer.
- Do not infer clock offset from sequential SSH response timestamps. Use NTP/PTP
  status or a concurrent measurement method that accounts for round-trip time.
- A common future start time is generated after preparation, with a minimum
  margin check immediately before concurrent sender launch. If the margin is
  insufficient, regenerate and redistribute both Station configs.
- All enabled Stations participate in the same repetition. Never combine a
  successful Station from one attempt with another Station from a later run.
- Multi-host fixtures use the source-port pair pinned by the selected passing
  profile and record it in generated configs and the manifest. Before any
  receiver ring is created, the controller probes each source IP/port on its
  sender host. An occupied bind endpoint is `ENV_BLOCKED`; a bind race detected
  when the sender starts has the same classification and still aborts the
  complete observation.

## Astronomy observation semantics

Tests must preserve the failure semantics of a continuous astronomical
observation, not merely exercise isolated commands:

- every configured Station is required to enter the same observation; if any
  physical sender process cannot start, terminate already-started senders and stop the
  receiver, unpack worker and consumer for that transfer;
- after the common start, monitor all Station senders concurrently; an early
  abnormal exit from any sender aborts the whole transfer immediately rather
  than waiting for the remaining sender to finish;
- a missing Station process is an observation-level failure and must not be
  accepted as a lower-antenna-count observation or combined with data from a
  different attempt;
- individual late, missing, duplicate or bad packets during an otherwise live
  observation follow the configured zero-fill and counter policy. They are
  distinct from a Station that failed to participate, and acceptance uses the
  configured error-rate thresholds.

## Performance reporting

Use two explicit compute-consumer modes:

- `dbdisk` is the correctness mode. It materializes the compute transfer so the
  test can inspect the PSRDADA header, exact data byte count and known TFPA
  sample bytes.
- `dbnull` is the performance mode. Run `dada_dbnull -k KEY -s -z -q` so the
  compute ring is drained with direct shared-memory block access and no disk
  write. Require the consumer to reach EOD and exit zero. In this mode output
  correctness is established by the preceding `dbdisk` gate plus exact
  receiver/unpack reconciliation for every measured run.

For receiver/unpack-only tests, use `task8c_rate_point.py --pipeline-stage
unpack --compute-consumer dbnull`. The consumer attaches directly to the
compute ring; no output ring, `pipeline_worker`, H2D, GPU algorithm or D2H is
part of that result. Set `--missing-wait-ms` and
`--station-skew-reserve-ms` independently. The controller derives
`window_blocks = 1 + missing-wait blocks + Station-skew-reserve blocks` from
the requested aggregate rate and block geometry before creating either ring;
the worker receives the missing-wait horizon explicitly.

Multi-Station sender acceptance uses the checked-in Observation inputs
`config/testing/multi-station-sender-small.json` and
`config/testing/multi-station-sender-production.json`. The first is the only
network functional gate (warm-up + three measured repetitions); the second is
a production-geometry sender/receive/unpack pressure plan and must not silently
replace an accepted two-Station baseline. For `A=469,F=4,P=1` at 1 MHz per F,
report 30.016 Gbps astronomical sample payload separately from the 30.2505
Gbps UDP datagram payload used by sender pacing. The two physical sender hosts
must run exactly one process/socket each, partition Stations 235/234, retain
the fixed baseline source ports, and reconcile every Station's group count.

For an `rdma2dada` receive-limit test, use `--pipeline-stage receive
--compute-consumer dbnull`. This mode creates only the raw ring, attaches
`dada_dbnull -s -z` directly to it, starts `rdma2dada`, and starts the Station
senders after receiver readiness. It does not create compute/output rings or
start unpack/GPU workers. Result directories use the
`rdma2dada-<rate>Gbps-<duration>s-<UTC>` prefix.

Do not compare a `dbdisk` throughput result with a `dbnull` result as if they
measured the same pipeline boundary; always report the consumer mode.

### ATFP full-pipeline wire-rate campaign

Formal performance acceptance uses only `scripts/atfp_throughput_campaign.py`
with `config/testing/atfp-throughput-campaign.json` and
`config/testing/atfp-throughput-observation.json`. The path is UDP input, raw
ring, ATFP unpack, compute ring, CUDA conversion/transpose and beamform, output
ring, then `dada_dbnull -s -z -q`.

The historical throughput observation remains the
`SYNCHRONOUS_DIRECT/1` comparison baseline. Multi-stream experiments use
`config/testing/atfp-throughput-observation-staged.json`, whose only intended
execution-policy difference is `STAGED_PIPELINE/3`; do not overwrite the
baseline file or compare runs with unrecorded geometry/placement differences.

The target is physical untagged-IPv4 Ethernet line rate. Fixed points are 1,
5, 10, 20, 30, 35 and 40 Gbps. Every point runs one 30-second warm-up and three
30-second measurements. After the first failure, bisect to a 0.5-Gbps maximum
pass/fail interval. Forty Gbps is an upper probe, not a mandatory pass.

The CLI requires explicit qths and sender Release directories plus a supplied
source SHA256 manifest and must never fall back to `build-linux`. Run
`--preflight-only` before `--execute`; hand-written single-rate commands are
diagnostic only.

For every measured rate and repetition, record:

- configured and actual payload rate for every sender;
- scheduled, sent, retried and failed packets plus batch/overrun counters;
- NIC, CQ and receiver accepted/drop/error deltas;
- raw-ring and compute-ring throughput/backpressure;
- unpack complete/incomplete/missing/duplicate/late/bad-header counters;
- CPU affinity/utilization and NUMA placement;
- producer→receiver→unpack→consumer count reconciliation;
- header, Station-to-A mapping, TFPA sample and EOD verification.

Report median, minimum, maximum and spread across measured repetitions. A rate
is stable only when every repetition satisfies correctness gates and the
documented rate tolerance. The first saturated component must be supported by
logs and counters, not inferred from sender rate alone.

For every physical-wire rate run, capture receiver NIC counters immediately
before the receiver-side processes start and immediately after both senders
finish. Resolve the Linux netdev from the configured RDMA device through
sysfs, and preserve both `/sys/class/net/<netdev>/statistics` and
`ethtool -S <netdev>` snapshots plus their run-scoped deltas in the run JSON.
Counter snapshots are required evidence: a missing tool, ambiguous RDMA-to-
netdev mapping, empty snapshot, interface change, or counter reset must be
reported explicitly and must not be interpreted as zero packet loss.

## Lessons retained from Task 8C

Task 8C exposed failures in nested SSH quoting, temporary host-key handling,
missing destination directories, sequential sender launch, premature
`start_utc`, SSH-based clock inference, unimplemented controller paths, false
self-test claims, swallowed exceptions and cleanup overwriting failure state.

These are harness failures, not evidence that UDP sender, RDMA ingest or VDIF
unpack failed. The policy above exists so the same mistakes become automated
regression cases and cannot recur as undocumented manual knowledge.

Only `HF` is the controller. Remote multi-host acceptance uses
`scripts/task8c_rate_point.py` from `HF`; attempting `qths1 -> qtpulsar*` is a
harness topology error, not an environment requirement or product failure.

The versioned controller accepts one observation JSON and the
`observation_config_compile` binary. It verifies the compiler artifact
manifest, transfers the same resolved plan to `rdma2dada` and
`vdif_unpack_worker`, and derives ring creation and acceptance counters from
that plan. It must not generate runtime `pipeline.json`, `packet.json` or
`worker.json` geometry files. Run `--preflight-only` before `--execute`; the
preflight follows the same compiler, synchronization, binary and endpoint gate
but creates no ring, capability or data process.

For performance runs the controller must pass its aggregate payload target to
the compiler through `--budget-payload-gbps`. The resulting validation report
must retain both the Observation-derived payload rate and the overridden test
rate with `rate_source=PERFORMANCE_OVERRIDE`. Before creating rings, preserve
and validate the 20% service deadline, per-stage rates, combined sequential
H2D+D2H bytes/rate, planned VRAM, recommended free VRAM and the explicit
runtime/workspace exclusion. An isolated kernel rate or available VRAM alone
does not satisfy this full-chain feasibility gate.
Both receiver-side and sender-side Release build directories are explicit
formal-run arguments. The controller must not silently select `build-linux`
for either `rdma2dada`/`vdif_unpack_worker` or `fpga_sender_sim`.

Controller process wrappers must forward `SIGTERM`/`SIGINT` to the real child
process and persist the child's exit status. Recording only a shell-wrapper PID
is invalid: terminating that wrapper can orphan `rdma2dada`, prevent its
signal handler from publishing raw-ring EOD, and leave every downstream worker
waiting indefinitely. This signal-forwarding path requires an executable
regression test before a formal finite-transfer run.

Receiver readiness must use the flushed `Receive threads ready` marker, which
is emitted only after the direct CQ poll/repost thread is running and any
requested affinity has been verified. The earlier resource marker
`Initialization complete, ready to start` is not sufficient. A readiness
timeout must print every managed
process PID, running state, recorded exit code and log tail; a silent timeout is
a harness failure and provides no product evidence.

The direct receive path fixes one destination-only flow, one QP/CQ/thread,
NSGE=2, a 42-byte header scratch SGE and two outstanding PSRDADA blocks. Its
tunable defaults are `--poll-batch 32 --recv-wr-num 1024`. The current qths1
Node-1 unpack profile uses receiver poll CPU 13, coordinator 14, parser workers
15–18, compute writer 19 and sink 20. These values belong to the versioned
runner invocation and are not hard-coded product defaults. Receive-only or
full-pipeline profiles may use a different non-overlapping map, but must record
and verify the effective affinity.

Failure-time diagnostic collection and resource cleanup are reported
separately. A missing artifact that was never expected to exist after an early
worker failure belongs in `diagnostic_errors`; it does not turn an otherwise
verified process/ring/capability cleanup into `CLEANUP_RESULT=FAIL`.

Generated bundle validation must follow the selected pipeline topology. A
GPU-only run does not create receiver, NIC-capture or sender artifacts, so its
configuration gate must neither hash nor transfer those files. Validate only
the files and processes declared by `PipelineTopology`; an unconditional check
for an artifact absent from that topology is `HARNESS_FAIL` and requires a
`prepare_configs` regression test before the formal run is retried.
Ring-key resolution follows the same rule: resolve only the keys named by the
selected topology. Building an eager raw/compute/output map is invalid because
receive-only artifacts intentionally have no output ring.
If a `dada_db` owner exits during prepare, report the affected ring and append
the tail of its ring log before returning the failure. Do not register or
destroy that ring as controller-owned until prepare has confirmed the owner is
alive; diagnostic visibility must not weaken resource-ownership safety.

Do not diagnose a run from a transient state snapshot while the controller is
still active. The compact run JSON must retain the complete state timeline,
launcher argv/PID/PGID and sender process ledger with host, Station, SSH argv,
PID, timestamps, exit code and combined output. SSH exit code 255 is a harness
transport failure; ordinary non-zero remote program exits remain product
failures. Persist this evidence before terminating peer Stations and cleanup.

`dada_db -p` is not a read-only ring probe: it creates persistent DADA data and
header blocks. Never use it during Phase 0 or diagnostics. Inspect existing IPC
state with non-creating system evidence; if key attribution is unavailable,
report it as unknown. Use `dada_db -d -k KEY` only for an exact ring whose test
ownership has already been established.
