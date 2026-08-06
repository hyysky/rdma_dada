# Pipeline configuration

Runtime configuration uses strict JSON. `pipeline.example.json` is schema
version 1 and has four sections:

前端 packet 的静态 byte/bit 定义与观测配置分开，存放在
[`packet_formats/`](packet_formats/README.md)。其中包含 v1 JSON Schema、机器可读 profile
和 `packet_format_inspect` 的调用说明。Project VDIF v1 的 32-byte header 与 TFP payload
布局是正式 wire contract；profile 中的 `payload_bytes` 是可由具体观测替换的 record
几何值。

- `observation`: externally supplied observation geometry and start time.
- `packet`: raw record geometry and per-sample interval in microseconds.
- `ring_buffers`: current raw/compute block geometry.
- `disk`: optional `dada_dbdisk` sink. `blocks_per_file` counts complete ring
  blocks; `direct_io` controls the `dada_dbdisk -o` option.

Unknown or missing fields are rejected so a misspelled parameter cannot silently
fall back to a default. Integer fields must use integer JSON syntax.

`ring_buffers.records_per_block` 是所有阵元合计的 UDP record 数，必须能被
`observation.nant` 整除：

```text
packets_per_antenna_per_block = records_per_block / nant
T = packet.samples * packets_per_antenna_per_block
packet.payload_bytes = packet.samples * nchan * npol * (packet.nbit / 8)
compute_block_bytes = packet.payload_bytes * records_per_block
compute_resolution = nchan * npol * nant * (packet.nbit / 8)
payload_bytes_per_second = packet.payload_bytes * nant / packet_duration
raw_bytes_per_second = (packet.header_bytes + packet.payload_bytes)
                       * nant / packet_duration
```

`packet.nbit` 是完整复数 sample 的 bit 数；当前 Project VDIF/CI8 配置固定为 16。
`packet.samples`、`nchan`、`npol` 和 `packet.payload_bytes` 必须满足上述等式，否则在
创建 ring 前拒绝配置。

Convert the former `KEY=VALUE` file with:

```sh
scripts/convert_config_to_json.py \
  config/pipeline.example.conf \
  config/pipeline.example.json
```

The legacy launcher always started `dada_dbdisk`, so the converter writes
`"disk.enabled": true`. Change it to `false` when raw disk recording is not a
consumer of the ring.

`pipeline_worker.example.json` is a separate strict schema version 2 for the
processing worker. It contains:

- `rings`: hexadecimal input/output PSRDADA keys. The two keys must differ.
- `execution`: `CPU_REFERENCE` or `CUDA`, CUDA device and one-transfer mode.
- `input_geometry`: worker-local `F/A/P`、每个 UDP 的 payload/sample 数，以及每个
  阵元在一个 block 中贡献的 UDP 包数。
- `beamform`: NPY weight path/scale/identity, output beam count and FP32/TF32.
- `output`: one of `BEAMFORMED`, `POWER` or `STOKES`.
- `integration`: enable flag, positive integer length and `sum` or `mean`.

Schema version 1 remains readable as integration disabled with length 1. New
configs should use version 2. Integration may follow `POWER` or `STOKES`, not
`BEAMFORMED`, and the configured block `T` must be divisible by its length.

Relative `weights_file` paths are resolved from the worker JSON directory, not
from the process working directory. The example therefore expects
`config/weights/beamform_weights.npy`.

Worker block 几何为：

```text
T = samples_per_udp * udp_packets_per_antenna_per_block
input_block_bytes = F * A * P * T * sizeof(CF32)
udp_antenna_group_bytes = udp_payload_bytes * A
input_block_bytes % udp_antenna_group_bytes == 0
product_block_bytes = T * selected_output_frame_bytes
output_T = integration.enabled ? T / integration.length : T
output_block_bytes = output_T * selected_output_frame_bytes
```

创建 ring 前可输出全部派生尺寸：

```sh
build/pipeline_worker_config_inspect config/pipeline_worker.example.json
```
