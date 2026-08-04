# pipeline_worker

Planned process-level application that owns exactly one input ring and one
output ring. It will build a validated ordered module chain from JSON, publish
the transformed output header, process blocks with backpressure, and propagate
EOD. Algorithm implementations remain under `modules/` and never own rings.
