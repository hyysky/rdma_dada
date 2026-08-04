# pipeline_worker

`pipeline_worker` 是第一版独立处理进程：通过 JSON 中的两个十六进制 key 连接一个
输入 PSRDADA HDU 和一个输出 PSRDADA HDU。它读取每个 transfer 的 input header，
与配置参数共同生成 output header，然后逐个 data block 处理并传播 EOD。

算法模块不连接 ring。worker 持有 ring lock、host/device buffer、CUDA stream 和
模块生命周期。

## 当前支持的模块链

配置的 `output.product` 选择以下一条固定合法链：

| product | 模块链 | 输出布局 |
| --- | --- | --- |
| `BEAMFORMED` | Beamform | `TFPB/CF32` |
| `POWER` | Beamform → Power | `TFPB/F32` |
| `STOKES` | Beamform → Stokes | `TFBS/F32`，产物顺序为 `AA,BB,AB_REAL,AB_IMAG` |

Power 和 Stokes 是互斥的兄弟分支，不会串联。Stokes 要求 `NPOL=2` 和两个明确的
`POL_LABELS`。积分模块尚未接入。

## 参数来源

| 来源 | 参数 |
| --- | --- |
| input header block | `NCHAN`、`NPOL`、`NANT`、数据阶段/顺序/类型、内存位置、采样/字节率、`UTC_START` 等观测元数据 |
| worker JSON | input/output ring key、CPU/CUDA backend、CUDA device、`F/A/P/T` 的 UDP 几何、权重文件/scale/id、`NBEAM`、FP32/TF32、最终 product |
| ring 实例 | input/output header capacity 和 data block capacity |

未知 input header 字段会保留到 output header。worker 更新算法输出的 stage、order、
sample format、resolution、byte rate、transfer/file size、offset、权重标识和执行后端。

## Input header 契约

第一版从已经完成解包、重排和 CF32 转换的 host ring 开始。必填字段为：

```text
DATA_STAGE=CONVERTED
ORDER=TFPA
SAMPLE_FORMAT=CF32
MEMORY=HOST 或 PINNED_HOST
NCHAN=F
NPOL=P
NANT=A
RESOLUTION=F*P*A*8
BYTES_PER_SECOND=<正整数且为 RESOLUTION 的整数倍>
```

header 中的 `NCHAN/NANT/NPOL` 必须与 JSON `input_geometry` 的 F/A/P 完全一致。
配置是 block 规划依据，header 是上游实际数据声明，二者不一致时拒绝运行。

Stokes 还要求 `NPOL=2`、`POL_LABELS=<label0>,<label1>`。可选的
`TRANSFER_SIZE`、`FILE_SIZE` 和 `OBS_OFFSET` 如果存在，必须对一个完整 TFPA 时间帧
对齐；输出 header 按输入/输出 frame byte 比例缩放这些字段。

## JSON

完整示例见 [`config/pipeline_worker.example.json`](../../config/pipeline_worker.example.json)：

```json
{
  "schema_version": 1,
  "rings": {
    "input_key": "dada",
    "output_key": "dadb"
  },
  "execution": {
    "backend": "CUDA",
    "cuda_device": 0,
    "run_once": true
  },
  "input_geometry": {
    "nchan": 1,
    "nant": 4,
    "npol": 2,
    "udp_payload_bytes": 8192,
    "samples_per_udp": 512,
    "udp_packets_per_antenna_per_block": 4096
  },
  "beamform": {
    "weights_file": "weights/beamform_weights.npy",
    "weights_order": "FPAB2",
    "weights_id": "observation-v1",
    "weights_scale": 0.0078125,
    "nbeam": 90,
    "compute_mode": "TF32"
  },
  "output": {
    "product": "STOKES"
  }
}
```

ring key 按十六进制解析，可以写 `dada` 或 `0xdada`。相对权重路径以 JSON 所在目录
为基准。CPU reference 只允许 `compute_mode=FP32`。

`udp_packets_per_antenna_per_block` 表示每个阵元贡献的 UDP 包数，不是所有阵元合计
包数；所有阵元合计包数为 `A×udp_packets_per_antenna_per_block`。

## Data block 几何

```text
T                       = samples_per_udp
                          * udp_packets_per_antenna_per_block
udp_antenna_group_bytes = udp_payload_bytes * A
input_frame_bytes       = F * P * A * sizeof(CF32)
input_ring_block_bytes  = T * input_frame_bytes
beamformed_frame_bytes  = F * P * B * sizeof(CF32)
power_frame_bytes       = F * P * B * sizeof(F32)
stokes_frame_bytes      = F * B * 4 * sizeof(F32)

output_ring_block_bytes = T * selected_output_frame_bytes
input_ring_block_bytes % udp_antenna_group_bytes = 0
```

`F×A×P×T` 表示维度大小，内存线性顺序仍为 `TFPA`。input/output ring 的 data block
capacity 必须与配置计算值完全相等，否则 worker 在发布数据前退出，避免 short block
被 PSRDADA 误认为提前 EOD。

## 构建与运行

```bash
cmake -S . -B build-linux \
  -DBUILD_RDMA_PIPELINE=ON \
  -DUSE_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-linux --parallel

./build-linux/pipeline_worker_config_inspect \
  config/pipeline_worker.json
./build-linux/pipeline_worker config/pipeline_worker.json
```

先用检查工具得到 `INPUT_BLOCK_BYTES` 和 `OUTPUT_BLOCK_BYTES`，再分别使用
`dada_db -k <key> -b <bytes> ...` 创建两个 ring。`run_once=true`
时完成一个 input transfer/EOD 后退出；为 `false` 时重新获取 input read lock，等待下一次
transfer。`SIGINT`/`SIGTERM` 会请求在当前 PSRDADA 操作返回后停止。

## CUDA 第一版限制

- 每个 transfer 创建一条 worker-owned non-blocking stream。
- 独立的 `HostToDeviceModule`、算法 kernel 和 `DeviceToHostModule` 都提交到同一
  stream；两个传输模块不申请 buffer，也不自行同步。
- 提交 output ring block、释放 input block 前执行 `cudaStreamSynchronize()`，保证生命周期
  正确。
- 尚未实现双 buffer/event，因此当前版本没有跨 block overlap；后续优化不改变模块接口和
  header/block 契约。

macOS 只构建并测试 worker core；PSRDADA/CUDA 可执行文件需要在 Linux 服务器构建。
