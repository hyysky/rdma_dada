# Passing test profiles

Each file in this directory is an immutable host/topology baseline extracted
from a retained, reproducible PASS result. A profile is not created from chat
summaries, dry runs, or a manually reconstructed command.

Normal `--execute` and `--preflight-only` runs load a profile with
`--baseline-profile`. Explicit changes require `--experiment-name`; the
controller records every changed field before creating a ring or process.

GPU and full profiles must record `gpu_worker_cpu`, `sink_cpu_list` and
NUMA placement. A legacy profile may use one `numa_node`; split placement uses
`ingress_numa_node` for the raw ring/receiver and `processing_numa_node` for
unpack, compute/output rings, GPU worker and sink. Receive/unpack profiles must
record receiver, coordinator, parser, writer and sink placement plus queue
geometry, the preparation interval and both Station source ports.
Scheduler-default placement or suite-derived source ports are not an accepted
baseline.

The accepted qths1 ingress/unpack baseline is
`qths1-unpack-30gbps-60s-v1.json`. A `full` run may inherit this `unpack`
profile to keep the receiver/raw/unpack seam unchanged while adding GPU and
output-ring roles. Changes to inherited fields remain named experiments.

The production A=469 upstream profile is
`qths1-unpack-30p2505gbps-60s-a469-v1.json`. It is extracted from the accepted
60-second Full Power suite and covers only the shared receiver/raw/unpack
boundary. A single-worker comparison loads this profile and overrides only
`worker_cpu_list` under an explicit experiment name; the comparison result does
not replace the parallel baseline.

When no accepted profile exists, the only allowed formal bootstrap name is
`bootstrap-<pipeline-stage>-v1`. A bootstrap run is a candidate only. It becomes
an accepted profile after the remote result, process ledger, cleanup state, raw
evidence SHA256, and suite manifest all validate.

The accepted profile was extracted from the retained warm-up plus three
measured PASS suite named in its `source_result`; it was not reconstructed from
a chat summary.
