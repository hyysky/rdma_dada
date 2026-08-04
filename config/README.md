# Pipeline configuration

Runtime configuration uses strict JSON. `pipeline.example.json` is schema
version 1 and has four sections:

- `observation`: externally supplied observation geometry and start time.
- `packet`: raw record geometry and per-sample interval in microseconds.
- `ring_buffers`: current raw/compute block geometry.
- `disk`: optional `dada_dbdisk` sink. `blocks_per_file` counts complete ring
  blocks; `direct_io` controls the `dada_dbdisk -o` option.

Unknown or missing fields are rejected so a misspelled parameter cannot silently
fall back to a default. Integer fields must use integer JSON syntax.

Convert the former `KEY=VALUE` file with:

```sh
scripts/convert_config_to_json.py \
  config/pipeline.example.conf \
  config/pipeline.example.json
```

The legacy launcher always started `dada_dbdisk`, so the converter writes
`"disk.enabled": true`. Change it to `false` when raw disk recording is not a
consumer of the ring.
