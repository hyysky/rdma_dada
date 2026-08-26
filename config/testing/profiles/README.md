# Passing test profiles

Each file in this directory is an immutable host/topology baseline extracted
from a retained, reproducible PASS result. A profile is not created from chat
summaries, dry runs, or a manually reconstructed command.

Normal `--execute` and `--preflight-only` runs load a profile with
`--baseline-profile`. Explicit changes require `--experiment-name`; the
controller records every changed field before creating a ring or process.

GPU and full profiles must record `gpu_worker_cpu`, `sink_cpu_list` and
`numa_node`. Receive/unpack profiles must record receiver, coordinator,
parser, writer and sink placement plus queue geometry. Scheduler-default
placement is not an accepted baseline.

When no accepted profile exists, the only allowed formal bootstrap name is
`bootstrap-<pipeline-stage>-v1`. A bootstrap run is a candidate only. It becomes
an accepted profile after the remote result, process ledger, cleanup state, raw
evidence SHA256, and suite manifest all validate.

No qths1 profile is checked in yet because HF is currently unavailable. The
first recovery action is read-only extraction from the retained passing result,
followed by local profile creation and review.
