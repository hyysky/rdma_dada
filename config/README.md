# Pipeline configuration

## Unified observation configuration

`observation.example.json` is the schema-v1 user input for the ongoing unified
configuration migration; `observation-v1.schema.json` documents the same strict
contract. The production C++ parser rejects missing and unknown fields.

User-entered fields cover observation timing, ordered Station IDs, channel and
polarization selection, telescope/frequency metadata, packet samples, block and
ring policy, receiver endpoint, storage policy and algorithm selection. Paths
are resolved relative to the observation JSON. Byte sizes and DADA headers are
not user inputs; `observation_config_compile` derives them from this file and
the fixed wire profile.

Preflight only:

```sh
build/observation_config_compile \
  --config config/observation.example.json \
  --preflight-only
```

Generate a new atomic artifact directory:

```sh
build/observation_config_compile \
  --config config/observation.example.json \
  --output-dir observation-artifacts
```

The destination must not already exist. It contains the verified resolved
configuration, ring plan, 4096-byte `raw.header`/`unpacked.header`, validation
report and `MANIFEST.sha256`. When `processing.modules` is non-empty, the
compiler also invokes the production module header transforms and writes
`converted.header`, `beamformed.header` and the final `output.header`; every
generated file is covered by the manifest.

`station_ids` order defines the A axis. `duration_seconds`, explicit
`processing.conversion.scale` and beamformer `weights_scale` are decimal
strings so they can be parsed without binary floating-point rounding. The
conversion scale must also be representable as a positive FP32 value.
`metadata.telescope`, `bandwidth_hz` and
`center_frequency_hz` are required and will propagate to generated DADA
headers.

The older pipeline/unpack/worker JSON examples below remain temporary migration
fixtures until all consumers use the resolved observation plan.

Runtime configuration uses strict JSON. `pipeline.example.json` is schema
version 1 and has four sections:

前端 packet 的静态 byte/bit 定义与观测配置分开，存放在
[`packet_formats/`](packet_formats/README.md)。其中包含 schema-v2 JSON Schema、机器可读 profile
和 `packet_format_inspect` 的调用说明。Project VDIF v1 的 32-byte header 与 TFP payload
布局是正式 wire contract；schema-v2 profile 不包含观测相关的 payload 大小或轴长度。

- `observation`: externally supplied observation geometry and start time.
- `packet`: raw record geometry and per-sample interval in microseconds.
- `ring_buffers`: current raw/compute block geometry.
- `disk`: optional `dada_dbdisk` sink. `blocks_per_file` counts complete ring
  blocks; `direct_io` controls the `dada_dbdisk -o` option.

Unknown or missing fields are rejected so a misspelled parameter cannot silently
fall back to a default. Integer fields must use integer JSON syntax.

## VDIF unpack 配置

`vdif_unpack.example.json` 是 raw ring 到 compute ring 解包进程的严格配置：

- `rings` 使用 `0x` 前缀字符串指定输入、输出 PSRDADA key，两个 key 不能相同；
- `sources` 指向观测 pipeline 配置和 Project VDIF packet-format profile，相对路径以
  unpack JSON 所在目录为基准；
- `selection.first_channel_id` 是该 worker 处理的最小频率编号；
- `selection.antenna_map[A]` 按 A 轴顺序列出 Station ID，长度必须等于 `NANT`，且不允许重复；
- `window.blocks` 第一版至少为 2，表示私有环形窗口容纳的 compute block 数；
- `window.max_bytes` 是 payload-only 滑动窗的硬内存上限；
- `output.memory` 当前固定为 `HOST`，输出继续写入 host PSRDADA ring。
- `runtime.run_once=false` 时每个 transfer 完成后重新等待下一次 header；功能测试可设为
  `true`，处理一个 transfer 后正常退出。

第一版 unpack worker 遵循当前已确认的 `PKT_NBIT=16`，即每个复数 sample 为
`int8 real + int8 imag`（`CI8`）。Project VDIF codec 和 sender 已保留 CI16 能力，但在
观测侧最终确定位宽并扩展 pipeline block/header 契约前，unpack 配置会明确拒绝 CI16，
不会静默按错误字节宽度运行。

示例使用 `vdif_unpack.pipeline.example.json`，其 packet 几何与
`packet_formats/frontend.example-v1.json` 一致。窗口不复制 32-byte VDIF header：

```text
groups_per_raw_block = records_per_block / NANT
group_bytes = packet_payload_bytes * NANT
window_bytes = window.blocks * records_per_block * packet_payload_bytes
compute_block_bytes = records_per_block * packet_payload_bytes
```

窗口内部按 `[A,circular_group,T,F,P]` 排列，不保存 packet header；输出 compute block
采用 `ORDER=ATFP`、`LAYOUT_SCOPE=BLOCK`。packet-format profile 只描述 wire TFP，不包含
下游 `output_order`。

## FPGA sender simulator 配置

`fpga_sender_sim.example.json` 描述单个 Station 的 UDP/Project VDIF v1 发送实例：

- `destination` 只限定接收端数值 IPv4、UDP 端口和 IPv4 path MTU；程序不限定源地址；
- `station.station_id` 必须与 unpack 的 `antenna_map[A]` 中某一项一致；
- `packet` 给出频段、TFP 几何、8/16-bit 分量和十进制字符串 `sample_interval_ps`；
- 示例测试间隔为 `1000000 ps = 1 us`，每 packet 包含 512 个时间 sample；
- `time.mode` 是 `BURST` 或 `REALTIME`；多服务器 REALTIME 测试必须使用相同的未来
  `start_utc`、reference epoch、start seconds 和 group count；
- `faults` 可确定性注入 drop、duplicate 和 invalid-header group。

`fpga_sender_sim.paced.example.json` 是严格 schema v2 高速 UDP 配置：

- `source.ip/port` 必须显式给出，且每个 Station process 使用不同 source port；
- `time.mode` 固定为 `PACED`，所有发送端使用共同未来 `start_utc`；
- `transmit.target_gbps` 表示完整 UDP datagram payload（VDIF header + data）的 Gbps；
- `batch_packets` 取值 `1..64`，Linux 使用 `sendmmsg()`；
- `payload_mode` 为 `REPEAT_TEMPLATE` 或 `DETERMINISTIC`；
- 所有 Station 等速，控制端不支持权重或单 Station override。

Task 8C controller 对多 Station Observation 生成 schema v3 配置，不另存一份手工示例：

- `station.station_ids` 是 Observation 中 Station ID 的有序、非空、无重复子集；
- qtpulsar1/2 各使用一个固定 source IP/port 和一个 socket；
- `time.group_count` 是时间组数，总 record 数为 `group_count × station_ids.size()`；
- 组内发送全部本机 Station，下一组轮换起始 Station；
- `transmit.target_gbps` 仍是完整 VDIF record（32-byte header + data）的速率，不是
  仅天文采样 data 的速率。

版本化多 Station 网络测试输入位于：

- `testing/multi-station-sender-small.json`：`A=4,F=4,P=1`，每个 F 为 1 MHz，
  用于低速 warm-up+3 流程验收；
- `testing/multi-station-sender-production.json`：`A=469,F=4,P=1`，每个 F 为
  1 MHz，仅用于 sender/receive/unpack 压力计划；GPU `B=350` 权重仍由
  `generate_gpu_pressure_fixture.py` 独立生成。该生成器默认产出
  `A=469,F=4,P=1,B=350` 的 Beamform→Power→MEAN Integration（K=128）
  Full Observation；传入 `--product coherency` 时产出输入规模相同的
  `A=469,F=2,P=2,B=350` Beamform→Stokes→MEAN Integration 配置。两种配置
  都使用 `STAGED_PIPELINE/3`，不改变 sender/unpack 配置。

`NPOL=2` 使用项目固定极化顺序 `X,Y`。Observation 无需重复配置标签；配置编译器
自动将 `POL_LABELS X,Y` 写入 RAW header，后续 unpack 和 GPU stage header 原样传播。

生产几何的天文采样 payload 为 30.016 Gbps；计入每个 packet 的 32-byte VDIF
header 后，sender 的 UDP datagram payload 目标为 30.2505 Gbps。controller 以后者
调度与验收，结果同时记录两种口径，禁止互换。

完整 `station_ids` 只保存在 Observation/Resolved Plan 中：sender 用它分片，unpack
用它建立 Station ID→ATFP A 轴映射。映射完成后的 ring header 不重复保存
`STATION_IDS`，只保存 `NANT` 和配置/几何身份；因此固定 4096-byte header 不随阵元数
线性膨胀。

IPv4 UDP record 必须满足：

```text
payload_bytes = T * F * P * 2 * (component_bits / 8)
record_bytes = 32 + payload_bytes
record_bytes <= path_mtu - 20 - 8
record_bytes <= 65507
```

fault 列表必须严格递增、不重复、不越界且彼此不重叠，避免同一 group 同时定义互相冲突
的发送行为。

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

`pipeline_worker.example.json` is a legacy module-test schema and is not the
application entry point. The application consumes the compiled
`resolved_observation.json`. The legacy file is a separate strict schema for
processing worker. It contains:

- `rings`: hexadecimal input/output PSRDADA keys. The two keys must differ.
- `execution`: `CPU_REFERENCE` or `CUDA`, CUDA device and one-transfer mode.
- `input_geometry`: worker-local `F/A/P`、每个 UDP 的 payload/sample 数，以及每个
  阵元在一个 block 中贡献的 UDP 包数。
- `beamform`: NPY weight path/scale/identity, output beam count and FP32/TF32.
- `output`: one of `BEAMFORMED`, `POWER` or `STOKES`.
- `integration`: enable flag, positive integer length and `sum` or `mean`.

Schema versions 1–3 remain readable for module regression tests; new application
runs must not create this independent worker JSON. Integration may follow
`POWER` or `STOKES`, not `BEAMFORMED`, and block `T` must be divisible by its
length.

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
