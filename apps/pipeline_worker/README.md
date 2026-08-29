# pipeline_worker

`pipeline_worker` 是独立处理进程：它只读取由
`observation_config_compile` 生成的 `resolved_observation.json`，通过其中的
`rings.compute_key` 和 `rings.output_key` 连接一个输入 PSRDADA HDU 和一个输出
PSRDADA HDU。它先校验 input header 与编译器生成的 `unpacked.header` 完全一致，校验
两个实际 ring 的 header capacity、data block capacity 和 block 数，再将运行时生成的
output header 与编译器生成的最终 header 完全比对，然后逐个 data block 处理并传播 EOD。

算法模块不连接 ring。worker 持有 ring lock、host/device buffer、CUDA stream 和
模块生命周期。

可选参数 `--metrics-json PATH` 在 transfer 结束时原子写入结构化指标，包括处理 block
数、输入/输出 bytes、每 block 总 service time、output-ring wait，以及同一 CUDA stream
上的 H2D、算法链、D2H 累计时间。该参数不增加第二条 stream，也不引入已有逐 block
同步之外的额外同步；测试控制器用这些字段核对每个 compute block 都产生一个 output
block。旧的一参数调用保持兼容。

固定前缀为 `H2D -> ComplexConvert`：输入 ring 的块级 `ATFP/CI8` 先在 GPU 上融合
完成物理转置、CI8→CF32 和 scale，得到独立的 `TFPA/CF32` buffer，再进入以下算法链。

## 当前支持的模块链

配置的 `output.product` 选择以下一条固定合法链：

| product | 模块链 | 输出布局 |
| --- | --- | --- |
| `BEAMFORMED` | ComplexConvert → Beamform | `TFPB/CF32` |
| `POWER` | ComplexConvert → Beamform → Power | `TFPB/F32` |
| `STOKES` | ComplexConvert → Beamform → Stokes | `TFBS/F32`，产物顺序为 `AA,BB,AB_REAL,AB_IMAG` |

Power 和 Stokes 是互斥的兄弟分支，不会串联。Stokes 要求 `NPOL=2`；Project
Observation v1 的偏振顺序固定为 `X,Y`，配置编译器自动写入 `POL_LABELS X,Y`。
`integration.enabled=true` 时，Power 或 Stokes 后追加
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
`TRANSFER_SIZE` 用于严格几何和输出字节规划；PSRDADA reader 本身始终读取到 compute
ring 的 EOD。这样当有限观测恰好结束于完整 block 边界时，不会因 reader 先达到声明
字节数、producer 后发布 EOD 而误开一个带非零 `OBS_OFFSET` 的伪 continuation transfer。

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
    "cuda_pipeline": {
      "mode": "STAGED_PIPELINE",
      "inflight_blocks": 3
    },
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

`processing.cuda_pipeline.mode` 可选：

- `SYNCHRONOUS_DIRECT`：单 stream 基线，`inflight_blocks` 必须为 1；输入
  ring 直接 H2D，D2H 直接写 output ring。
- `STAGED_PIPELINE`：1–4 个有界 slot，每个 slot 独占 non-blocking CUDA
  stream、pinned output staging 和 device 中间 buffer；worker 在 transfer 生命周期
  用 `dada_cuda_dbregister` 注册 compute ring，H2D 直接读取 ring block；reader 按 block
  序号提交，单 writer 严格按序写 output ring，因此 CUDA 完成乱序不会造成
  ring block 乱序。

省略 `cuda_pipeline` 时保持兼容基线 `SYNCHRONOUS_DIRECT/1`。staged 模式的
slot wait、writer wait、完成乱序、ring 注册、显存和 pinned output 预算均写入 worker
metrics JSON；兼容字段 `input_staging_bytes` 在直注册路径必须为 0。

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
  --config config/observation.json \
  --output-dir run/observation
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
- `SYNCHRONOUS_DIRECT/1` 在提交 output ring block、释放 input block 前执行
  `cudaStreamSynchronize()`，保留为未优化对照路径。
- `STAGED_PIPELINE/N` 在 transfer 开始时注册整个 compute ring；每个 slot 使用独立
  non-blocking stream、device buffers、pinned output 和完成事件。H2D 直接读取当前
  compute-ring block，输入 lease 保留到该 slot 的 H2D 完成；单一 writer 等待下一
  sequence 的完成事件，再复制并提交 output ring block，因此跨 block overlap 不改变
  header、block 或有序发布契约。

## GPU block deadline 与容量预检

编译 Observation 时会把 `gpu_pipeline_budget` 写入 `validation_report.json`。普通观测使用
Observation 自身推导的 payload 速率；吞吐测试必须显式传入目标速率，例如：

```bash
./build-linux/observation_config_compile \
  config/testing/atfp-throughput-observation-staged.json run/full-30g \
  --budget-payload-gbps 30
```

历史 30 Gbps 小几何测试链为 Beamform → Power → Integrate(K=128, MEAN)。compute block
含 6,553,600 个时间采样，能被 128 整除；积分后 `T=51,200`。compute、Beamform、
Power 和最终 output block 分别为 52,428,800、209,715,200、104,857,600 和 819,200 B，
block 到达间隔 13,981,013 ns。保留 20% 余量后，单 stream 整链 deadline 为
11,184,810 ns；H2D+D2H 合计 53,248,000 B/block，最低合计 host/device 传输速率为
4,760,742,472 B/s。计划显存为 577,536,064 B（含 64 B 权重及算法 scratch），建议启动时
至少有 693,043,277 B 空闲显存；该值不含 CUDA/cuBLAS/驱动 workspace。

同一测试配置的 host ring 容量由 `ring_plan.json` 单独给出：raw 845,414,400 B、compute
419,430,400 B、output 6,553,600 B，合计 1,271,398,400 B。ring 容量只提供突发
缓冲时间，不计入显存，也不能弥补下游稳态服务率不足。

服务器 full-chain 功能诊断已在 1 Gbps 精确闭合。早期单次 30 Gbps 诊断中 GPU 处理 2,130
个 block，平均 service time 为 16.243 ms，超过 13.981 ms 的 block 到达间隔和
11.185 ms 的安全 deadline；平均 H2D/算法/D2H 分别约 12.077/3.873/0.260 ms，output
wait 平均约 0.0013 ms。该结果定位了旧 pageable-ring/逐 block 同步路径的性能问题。
compute-ring CUDA 直注册和三 slot staged pipeline 实现后，双 Station 小几何 full-chain
已通过 30 Gbps、60 秒、1 warm-up + 3 measured 原型门禁。生产 Full Power
`A=469,F=4,P=1,B=350` 与 Full coherency/Stokes `A=469,F=2,P=2,B=350` 随后也分别
完成约 30 Gbps、60 秒、1 warm-up + 3 measured 正式门禁；权威 suite 为
`full-30.2505Gbps-60s-20260829T033639Z` 和
`full-30.2505Gbps-60s-20260829T075447Z`。

GPU-only 生产几何压力由 repository-owned `gpu_pressure_writer` 驱动：它原样发布编译器
生成的 `unpacked.header`，按单调时钟 deadline 写完整 compute block，并记录精确计数、
等待、迟到和 EOD。测试分别复用 Power `A=469,F=4,P=1,B=350` 与 coherency/Stokes
`A=469,F=2,P=2,B=350` 配置，可匹配比较 direct/1 与 staged/N；该测试不经过 Station-ID
聚合，也不能替代多 Station sender 驱动的 full-stage 验收。

Power 生产几何的正式匹配结果为 direct/1 suite
`gpu-30Gbps-60s-20260829T113850Z` 与 staged/3 suite
`gpu-30Gbps-60s-20260829T114602Z`。两者均完成 60 秒、1 warm-up + 3 measured；staged
记录 `max_inflight=3`，证明三个 slot 在压力链中实际并发使用。

在 direct/1 中，metrics 的 `blocks`、输入/输出 bytes、transfer elapsed 及 CUDA
H2D/algorithm/D2H totals 是处理闭合证据；submitted/completed/published、max-inflight
和 active-window 是 staged 调度器指标，direct 中为 0 属于未启用该调度器，而不是未处理
数据。显存和 deadline 预算以 compiler `gpu_pipeline_budget` 为权威来源。

macOS 只构建并测试 worker core；PSRDADA/CUDA 可执行文件需要在 Linux 服务器构建。
