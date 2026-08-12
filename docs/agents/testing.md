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
below `send_n` and the final partial raw block. Acceptance requires
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

## Required run artifacts

Each run directory contains at least:

Rate-point result directories use
`<pipeline-stage>-<aggregate-rate>Gbps-<duration>s-<UTC timestamp>` so rate and
time are visible without opening the manifest.

- `manifest.json`: Git commit, source manifest, binary origin, host names,
  toolchain versions, configuration hashes, command arguments and run ID;
- `state.json`: current orchestration phase and owned resource identifiers;
- `result.json`: immutable test outcome, failure classification, counters,
  rates and acceptance checks;
- separate stdout/stderr logs for controller, receiver, worker, consumer and
  every sender;
- cleanup evidence showing the disposition of every PID, ring key, capability
  and temporary path owned by the run.

Git is managed only in the local development worktree. Remote test hosts do not
need a Git executable and test controllers must not invoke Git there. The
development task supplies source SHA256 values; remote manifests record and
compare those values together with generated configuration and binary SHA256.

`result.json` is written before cleanup and augmented with cleanup status; the
test outcome is never replaced by a final `CLEANED` state. Python controllers
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
8. retain the original failure after cleanup and always write `result.json`;
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
and preparation timing. An optional `--worker-cpu-list` wraps only the worker
with `/usr/bin/taskset -c`; omission preserves normal scheduling. The rendered
argv and observed affinity are retained with the run artifacts.

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
- Multi-host fixtures derive a distinct pair of UDP source ports from the
  unique run identity and record them in the generated configs and manifest.
  Before any receiver ring is created, the controller probes each source
  IP/port on its sender host. An occupied bind endpoint is `ENV_BLOCKED`; a
  bind race detected when the sender starts has the same classification and
  still aborts the complete observation.

## Astronomy observation semantics

Tests must preserve the failure semantics of a continuous astronomical
observation, not merely exercise isolated commands:

- every configured Station is required to enter the same observation; if any
  Station sender cannot start, terminate already-started senders and stop the
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
`ethtool -S <netdev>` snapshots plus their run-scoped deltas in `result.json`.
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

The receiver-focused `rdma_receiver_integration.sh` additionally assumes that
the machine executing it can SSH directly to both sender hosts. The deployed
topology does not satisfy that assumption: only `HF` is the controller. Remote
multi-host acceptance therefore uses `scripts/task8c_rate_point.py` from `HF`;
copying the shell test to `qths1` and attempting `qths1 -> qtpulsar*` is a
harness topology error, not an environment requirement or product failure.

The versioned controller accepts one observation JSON and the
`observation_config_compile` binary. It verifies the compiler artifact
manifest, transfers the same resolved plan to `rdma2dada` and
`vdif_unpack_worker`, and derives ring creation and acceptance counters from
that plan. It must not generate runtime `pipeline.json`, `packet.json` or
`worker.json` geometry files. Run `--preflight-only` before `--execute`; the
preflight follows the same compiler, synchronization, binary and endpoint gate
but creates no ring, capability or data process.
Both receiver-side and sender-side Release build directories are explicit
formal-run arguments. The controller must not silently select `build-linux`
for either `rdma2dada`/`vdif_unpack_worker` or `fpga_sender_sim`.

Controller process wrappers must forward `SIGTERM`/`SIGINT` to the real child
process and persist the child's exit status. Recording only a shell-wrapper PID
is invalid: terminating that wrapper can orphan `rdma2dada`, prevent its
signal handler from publishing raw-ring EOD, and leave every downstream worker
waiting indefinitely. This signal-forwarding path requires an executable
regression test before a formal finite-transfer run.

Receiver readiness must use the flushed initialization marker
`Initialization complete, ready to start`, not the later buffered
`RDMA receiver running` message. A readiness timeout must print every managed
process PID, running state, recorded exit code and log tail; a silent timeout is
a harness failure and provides no product evidence.

Failure-time diagnostic collection and resource cleanup are reported
separately. A missing artifact that was never expected to exist after an early
worker failure belongs in `diagnostic_errors`; it does not turn an otherwise
verified process/ring/capability cleanup into `CLEANUP_RESULT=FAIL`.

`dada_db -p` is not a read-only ring probe: it creates persistent DADA data and
header blocks. Never use it during Phase 0 or diagnostics. Inspect existing IPC
state with non-creating system evidence; if key attribution is unavailable,
report it as unknown. Use `dada_db -d -k KEY` only for an exact ring whose test
ownership has already been established.
