# pipeline_worker

`pipeline_worker` 是独立处理进程：它只读取由
`observation_config_compile` 生成的 `resolved_observation.json`，通过其中的
`rings.compute_key` 和 `rings.output_key` 连接一个输入 PSRDADA HDU 和一个输出
PSRDADA HDU。它先校验 input header 与编译器生成的 `unpacked.header` 完全一致，校验
两个实际 ring 的 header capacity、data block capacity 和 block 数，再将运行时生成的
output header 与编译器生成的最终 header 完全比对，然后逐个 data block 处理并传播 EOD。

算法模块不连接 ring。worker 持有 ring lock、host/device buffer、CUDA stream 和
模块生命周期。

固定前缀为 `H2D -> ComplexConvert`：输入 ring 的块级 `ATFP/CI8` 先在 GPU 上融合
完成物理转置、CI8→CF32 和 scale，得到独立的 `TFPA/CF32` buffer，再进入以下算法链。

## 当前支持的模块链

配置的 `output.product` 选择以下一条固定合法链：

| product | 模块链 | 输出布局 |
| --- | --- | --- |
| `BEAMFORMED` | ComplexConvert → Beamform | `TFPB/CF32` |
| `POWER` | ComplexConvert → Beamform → Power | `TFPB/F32` |
| `STOKES` | ComplexConvert → Beamform → Stokes | `TFBS/F32`，产物顺序为 `AA,BB,AB_REAL,AB_IMAG` |

Power 和 Stokes 是互斥的兄弟分支，不会串联。Stokes 要求 `NPOL=2` 和两个明确的
`POL_LABELS`。`integration.enabled=true` 时，Power 或 Stokes 后追加
TimeIntegrate；`BEAMFORMED` 不允许直接积分。

## 参数来源

| 来源 | 参数 |
| --- | --- |
| input header block | `NCHAN`、`NPOL`、`NANT`、数据阶段/顺序/类型、内存位置、采样/字节率、`UTC_START` 等观测元数据 |
| resolved observation plan | compute/output ring key、CPU/CUDA backend、CUDA device、`F/A/P/T`、整数转换 scale、权重文件/scale/id、算法链、积分长度/运算及全部派生 block/ring 几何 |
| 权重 NPY | `[F,P,A,B,2]`；`B` 维自动确定 `NBEAM`，F/P/A 和 dtype 必须与观测输入一致 |
| ring 实例 | input/output header capacity、data block capacity 和 block 数 |

header 不允许进程各自扩展：Observation 编译器通过生产模块的 header transform 生成
`unpacked.header`、`converted.header`、`beamformed.header` 和 `output.header`，worker
按这些字段逐项核对。算法变换会更新 stage、order、sample format、resolution、byte
rate、transfer/file size、offset、权重标识和执行后端。

## Input header 契约

worker 从 unpack 输出的 host compute ring 开始。必填字段为：

```text
DATA_STAGE=UNPACKED
ORDER=ATFP
LAYOUT_SCOPE=BLOCK
SAMPLE_FORMAT=CI8
SAMPLE_ENCODING=TWOS_COMPLEMENT
COMPONENT_ORDER=IQ
COMPONENT_NBIT=8
SAMPLE_NBIT=16
MEMORY=HOST 或 PINNED_HOST
NCHAN=F
NPOL=P
NANT=A
BLOCK_NTIME=T
RESOLUTION=F*P*A*2
RECORD_BYTES=OUTPUT_BLOCK_BYTES=T*F*P*A*2
BYTES_PER_SECOND=<正整数且为 RESOLUTION 的整数倍>
```

header 中的 `NCHAN/NANT/NPOL` 必须与 Resolved Plan 的 F/A/P 完全一致，且
`CONFIG_ID/GEOMETRY_ID` 必须匹配。Resolved Plan 是 block 规划依据，header 是上游
实际数据声明，任一不一致时都在获取 data block 前拒绝运行。

Stokes 还要求 `NPOL=2`、`POL_LABELS=<label0>,<label1>`。可选的
`TRANSFER_SIZE`、`FILE_SIZE` 和 `OBS_OFFSET` 如果存在，必须对一个完整 TFPA 时间帧
对齐；输出 header 按输入/输出 frame byte 比例缩放这些字段。启用积分时还必须有正数
有限值 `TSAMP`；所有上述 byte count 在产品变换和除以积分长度 `K` 时都必须能精确表示。

## 单一配置入口

用户只维护 [`config/observation.example.json`](../../config/observation.example.json)。
其中处理部分示例如下：

```json
{
  "rings": {
    "raw_key": "0x00d2",
    "compute_key": "0x00d4",
    "output_key": "0x00d6"
  },
  "processing": {
    "backend": "CUDA",
    "cuda_device": 0,
    "run_once": true,
    "conversion": {"scale": "0.0078125"},
    "modules": [
      {"type": "beamform", "weights_file": "weights/beamform.npy",
       "weights_order": "FPAB2", "weights_id": "observation-v1",
       "weights_scale": "0.0078125", "compute_mode": "TF32"},
      {"type": "stokes"},
      {"type": "integrate", "length": 128, "operation": "MEAN"}
    ],
    "output": {"sample_format": "AUTO"}
  }
}
```

`conversion.scale` 必须显式配置为正有限十进制字符串，不由 CI8 自动推导。
第一版 `output.sample_format` 只允许 `AUTO`：Beamform-only 自动为 `CF32`，Power、
Stokes 及其积分输出自动为 `F32`。output ring 的 block 数复用 compute ring block 数，
block bytes 由权重 B 维、所选产品和积分长度自动计算。

[`config/pipeline_worker.example.json`](../../config/pipeline_worker.example.json)
仅保留给模块级兼容测试，不再是 `pipeline_worker` 应用入口。

`udp_packets_per_antenna_per_block` 表示每个阵元贡献的 UDP 包数，不是所有阵元合计
包数；所有阵元合计包数为 `A×udp_packets_per_antenna_per_block`。

## Data block 几何

```text
T                       = samples_per_udp
                          * udp_packets_per_antenna_per_block
udp_antenna_group_bytes = udp_payload_bytes * A
input_frame_bytes       = F * P * A * sizeof(CI8 complex)
input_ring_block_bytes  = T * input_frame_bytes
converted_frame_bytes   = F * P * A * sizeof(CF32)
converted_block_bytes   = T * converted_frame_bytes
beamformed_frame_bytes  = F * P * B * sizeof(CF32)
power_frame_bytes       = F * P * B * sizeof(F32)
stokes_frame_bytes      = F * B * 4 * sizeof(F32)

product_block_bytes     = T * selected_output_frame_bytes
T_out                   = integration.enabled ? T/K : T
output_ring_block_bytes = T_out * selected_output_frame_bytes
input_ring_block_bytes % udp_antenna_group_bytes = 0
```

启用积分时还要求 `T % K = 0`。单进程中间 scratch 为 Beamform block；启用积分时
还需追加一个未积分 product block：

```text
scratch_block_bytes = beamformed_block_bytes + product_block_bytes
```

输入 `F×A×P×T` 的物理线性顺序为块级 `ATFP`；转换后的独立 buffer 才是 `TFPA`。
input/output ring 的 4096-byte header capacity、data block capacity 和 block 数必须与
Resolved Plan 完全相等，否则 worker 在取得 input data block 前退出，避免错误 ring 或
short block 被 PSRDADA 当作有效观测数据。

## 构建与运行

```bash
cmake -S . -B build-linux \
  -DBUILD_RDMA_PIPELINE=ON \
  -DUSE_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=86
cmake --build build-linux --parallel

./build-linux/observation_config_compile \
  config/observation.json run/observation
./build-linux/pipeline_worker \
  run/observation/resolved_observation.json
```

使用生成目录内的 `ring_plan.json` 创建 raw、compute 和 output ring；worker 只连接
compute 与 output 两个 ring。`run_once=true`
时完成一个 input transfer/EOD 后退出；为 `false` 时重新获取 input read lock，等待下一次
transfer。`SIGINT`/`SIGTERM` 会请求在当前 PSRDADA 操作返回后停止。

## CUDA 第一版限制

- 每个 transfer 创建一条 worker-owned non-blocking stream。
- 独立的 ATFP integer input、converted TFPA、module scratch 和 final output device
  buffer 不允许 alias。`HostToDeviceModule`、转换/算法 kernel 和
  `DeviceToHostModule` 都提交到同一 stream；两个传输模块不申请 buffer，也不自行同步。
- 提交 output ring block、释放 input block 前执行 `cudaStreamSynchronize()`，保证生命周期
  正确。
- 尚未实现双 buffer/event，因此当前版本没有跨 block overlap；后续优化不改变模块接口和
  header/block 契约。

macOS 只构建并测试 worker core；PSRDADA/CUDA 可执行文件需要在 Linux 服务器构建。
