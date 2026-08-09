# ATFP Pipeline Throughput Campaign Controller Design

## Objective

Create one versioned controller that reproducibly finds the maximum stable
physical Ethernet line rate of the baseline observation pipeline and identifies
whether the first sustained-throughput bottleneck is the network interface,
ATFP unpack, or the GPU processing chain.

The baseline chain is fixed for this campaign:

```text
two UDP Station senders
  -> rdma2dada / raw PSRDADA ring
  -> ATFP vdif_unpack_worker / compute ring
  -> H2D + ATFP-to-[TFP,A] transpose + CI conversion + scale
  -> beamform
  -> D2H / output ring
  -> dada_dbnull -s -z
```

Power, Stokes and integration modules are outside this baseline campaign and
will be measured in separate observation-mode campaigns.

## Formal Test Entry

The versioned campaign controller is the only formal acceptance entry. It
reuses the existing single-rate execution primitive but owns the complete
multi-rate lifecycle. Direct test-binary invocations, interactive command
sequences and temporary controllers are development diagnostics only and cannot
produce an accepted result.

Before any remote performance run, the controller must pass its local review,
unit tests, failure-path tests and dry-run inspection. Formal remote execution
starts only after those checks have been reviewed. The implementation phase
does not itself authorize a remote test.

## Rate Definition

`target_wire_gbps` means aggregate physical Ethernet line rate at the receiving
NIC. It is not VDIF payload rate or UDP socket byte rate. The initial campaign
points are:

```text
1, 5, 10, 20, 30, 35, 40 Gbps
```

Forty Gbps is an upper probe, not a mandatory passing target.

For the first implementation, the wire accounting model is untagged IPv4 over
Ethernet. For every UDP datagram it includes:

- the complete Project VDIF record sent through the socket;
- UDP, IPv4 and Ethernet headers;
- Ethernet FCS, preamble/SFD and inter-packet gap.

The controller records every byte component and the resulting conversion in
the campaign manifest. It divides the aggregate target equally between the two
mandatory Stations, then derives each sender's target VDIF-record rate. If the
observed network uses VLAN tagging, IPv6 or another framing model, Phase 0 must
reject the campaign until the accounting model is explicitly changed; it must
not silently report the socket rate as wire rate.

## Campaign Lifecycle

Phase 0, source synchronization verification, fresh Release build, paired
CMake/CTest gates and configuration preflight run once per campaign. Their
source, configuration, executable, library and environment identities are
stored in an immutable manifest.

Each rate point then owns fresh scoped rings, processes, logs and result files:

1. create and verify the required rings;
2. start `dada_dbnull -s -z` and the receiver-side pipeline;
3. verify every receiver-side process is ready;
4. start both Station senders against one common future start boundary;
5. run one 30-second warm-up;
6. recreate scoped runtime resources;
7. run three independent 30-second measured repetitions;
8. collect counters and logs before cleanup;
9. clean only resources owned by that repetition;
10. classify the rate point and decide the next point.

If either Station cannot start or exits abnormally, the controller immediately
stops both senders and the complete receiver-side pipeline. A later rate point
must never run after a failed lower fixed point.

Only one formal remote campaign may run at a time. A versioned controller lock
records the owner, campaign ID and start time. New delegated tests queue rather
than interrupt the active campaign unless the user explicitly requests a stop.

## Adaptive Search

The controller scans the fixed points in ascending order. After the first
failed point, it stops the fixed scan and performs bisection between the last
passing point and the first failing point.

Every bisection point receives the same one warm-up plus three measured
30-second repetitions. Search stops when the pass/fail interval is no wider
than 0.5 Gbps. The report contains both boundaries; it does not round the
failing boundary down or describe it as stable.

If 1 Gbps fails, there is no lower passing bound and the campaign reports that
the baseline pipeline cannot sustain its minimum requested point. If all fixed
points through 40 Gbps pass, the report states that no bottleneck was reached
within the tested range; it must not claim that 40 Gbps is the system maximum.

## Per-Stage Evidence

Every measured repetition records synchronized counters and timing for:

- sender: scheduled, sent, retried and failed packets; actual socket rate and
  derived physical wire rate for each Station;
- NIC: before/after packet, byte, discard, steering-miss, buffer and error
  counter deltas;
- receiver: accepted and published records, CQ tail/backlog, partial blocks,
  processing rate and raw-ring occupancy;
- unpack: accepted records, complete/incomplete/fully-missing groups, zero fill,
  processing time, output bytes, window state and raw/compute-ring wait time;
- GPU chain: H2D, transpose/conversion/scale, beamform and D2H time, bytes and
  effective throughput, without unconditional synchronization that changes the
  production path;
- pipeline: input/output accounting, maximum and time-series ring occupancy,
  backpressure duration, EOD state and process exit status;
- host: CPU/NUMA utilization, GPU utilization and relevant memory-transfer
  measurements over the repetition interval.

Counters must be captured as run-local deltas. Cumulative NIC counters without
a pre-run snapshot cannot be used to attribute a failure.

## Bottleneck Classification

The controller reports evidence and applies these ordered classifications:

1. **Physical network/NIC:** sender wire targets are met, but run-local NIC
   discard, steering-miss or buffer-drop deltas explain missing receiver input,
   or the physical interface cannot sustain the requested wire rate.
2. **ATFP unpack:** receiver input remains complete, raw-ring occupancy grows
   toward full, and unpack service throughput remains below the receiver input
   rate.
3. **GPU chain:** unpack output remains complete, compute-ring occupancy grows
   toward full, and GPU consumption remains below unpack output. H2D,
   transpose/conversion, beamform and D2H measurements identify the first slow
   GPU substage.
4. **Undetermined:** evidence is insufficient or more than one boundary is
   saturated without stage timing. The controller reports `UNDETERMINED` and
   requests a diagnostic campaign; it does not guess.

Increasing ring size is not accepted as a steady-state throughput fix. Ring
size may absorb bursts, but a passing rate requires stable occupancy rather
than eventual full-ring backpressure.

## Rate-Point Acceptance

A rate point passes only when all three measured repetitions pass. Every
repetition must satisfy:

- both mandatory Stations send the complete planned stream;
- actual derived wire rate is within the configured tolerance;
- receiver, unpack, GPU and output byte/record accounting matches exactly,
  except for explicitly recorded protocol zero fill;
- no unexplained unknown Station, bad header, duplicate, out-of-range or missing
  data exists;
- ring occupancy does not show sustained growth to full;
- every process reaches the expected EOD boundary and exits cleanly;
- runtime collection completes before scoped cleanup.

The controller reports median, minimum, maximum and spread for every relevant
measured throughput and latency metric. It never selects only the best run.

## Result Model

```text
campaign-id/
  campaign.json
  environment.json
  manifest.sha256
  build-and-gates/
  rate-01.000/
    warmup-01/
    measured-01/
    measured-02/
    measured-03/
    summary.json
  rate-.../
  bottleneck_report.json
  summary.json
```

The machine-readable status classes are:

- `ENV_BLOCKED`: a required observed environment facility is unavailable;
- `SYNC_FAIL`: synchronized sources or their SHA256 identities differ;
- `HARNESS_FAIL`: controller, generated configuration or orchestration fails;
- `PRODUCT_FAIL`: functional or numerical data contracts fail;
- `PERFORMANCE_FAIL`: contracts remain correct but sustained service rate is
  below the requested physical line rate;
- `PASS`: every acceptance condition passes.

`CLEANUP_RESULT` is independent and never overwrites the test result.

## Resume Rules

Resume is allowed only at a whole-rate boundary and only when the campaign
configuration, source SHA manifest, executable/library SHA manifest and Phase 0
environment identity still match. An interrupted or partially completed rate
point restarts from its warm-up; individual measured repetitions are not
reused. A changed identity starts a new campaign ID.

## Implementation Boundary

The first implementation changes only the versioned controller, its campaign
configuration/schema, automated controller tests and testing documentation.
It must reuse the existing resolved observation plan and single-rate execution
path. Product pipeline changes are not part of this controller task. If the
required GPU-stage metrics do not exist, the controller reports the missing
evidence and a later, separately reviewed instrumentation change supplies them.
