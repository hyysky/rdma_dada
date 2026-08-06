# Algorithm module contracts

本文档定义 RDMA/PSRDADA pipeline 第一版算法模块的数据布局、输入输出、
header 更新、组合约束和 `pipeline_worker` 调用规则。后续模块实现和测试应以本文档
为准。

整体进程与 ring 拓扑见
[PIPELINE_ARCHITECTURE.md](PIPELINE_ARCHITECTURE.md)。当前 C++ 接口原型见
[`stage.h`](../include/rdma_dada/pipeline/stage.h) 和
[`block.h`](../include/rdma_dada/pipeline/block.h)。这些接口仍需按照本文档补充
tensor 描述、模块计划和可变输出 block 几何。

## 1. 术语和轴定义

本文档使用以下轴名称：

| 轴 | 含义 |
| --- | --- |
| `T` | 一个 ring data block 内的时间 sample 数 |
| `F` | 频率或 channel 数，等于 `NCHAN` |
| `P` | 输入偏振数，等于 `NPOL` |
| `A` | 天线或阵元数，等于 `NANT` |
| `B` | 波束数，等于 `NBEAM` |
| `S` | 偏振相关产物数，当前固定为 4 |

`A` 已用于 antenna 轴，`B` 已用于 beam 轴。两路偏振信号在代码中使用
`P0/P1` 或 `X/Y`，不能再使用容易与轴混淆的变量名 `A/B`。如果仪器使用
偏振标签 `A/B`，通过 header 字段 `POL_LABELS=A,B` 保留。

## 2. 全局模块规则

1. 一个算法模块不创建、连接或销毁 PSRDADA ring。
2. 一个算法模块不管理进程生命周期，不启动 detached thread。
3. `pipeline_worker` 拥有一个输入 ring 和一个输出 ring，并在进程内组合模块。
4. 第一版所有模块遵循一个输入 block 对应一个输出 block。
5. 输入和输出 block 的字节数可以不同，但每次调用必须恰好提交一个输出 block。
6. 每个 block 必须包含整数个完整时间帧，不能在 sample、复数分量或 tensor
   frame 中间截断。
7. 模块启动时先验证 header、JSON 参数、数据类型、轴顺序、维度、内存位置和
   block 几何；验证失败时不能发布输出 header 或输出数据。
8. 未知的上游 header 字段必须原样保留。模块只更新自己负责的字段。
9. header block 每个 transfer 发布一次；data block 中不重复包含 ASCII header。
10. 模块顺序由 JSON 决定，但只有输入输出契约兼容的顺序才允许启动。

## 3. Tensor 和 block 描述

算法不能仅依赖 `void*` 和字节数判断输入。模块计划至少需要以下逻辑描述：

```cpp
enum class DataType {
    kComplexInt8,   // CI8: int8 real + int8 imag
    kComplexInt16,  // CI16: int16 real + int16 imag
    kComplexFloat32,// CF32: float real + float imag; CUDA 侧为 cuComplex
    kFloat32        // F32
};

struct TensorSpec {
    DataType datatype;
    std::string order;               // TFPA, TFPB or TFBS
    std::vector<std::uint64_t> shape;
    MemoryLocation location;
    int cuda_device;
};
```

`T` 由实际输入 block 字节数和单时间帧字节数推导；`F/P/A/B/S` 来自 header、
配置和模块输出计划。输出 ring 的 block capacity 由最终模块链在启动前计算。

## 4. Raw 复数 sample 格式

前端通常输出固定宽度的复数整数，例如 `8+8 bit`，即 I 8 bit、Q 8 bit；I 映射
复数实部，Q 映射复数虚部。
实际 component 位宽尚未最终确定，因此程序不能将 `CI8`、`CI16` 或 `cuComplex`
写死。

JSON 必须无歧义地描述 raw sample：

```json
{
  "sample_format": "CI8",
  "sample_encoding": "TWOS_COMPLEMENT",
  "component_order": "IQ",
  "endian": "LITTLE"
}
```

`CI8` 和 `CI16` 按类型定义固定表示有符号复整数，不再通过独立的 `signed` JSON
字段声明，避免类型与布尔标志互相矛盾。

派生关系：

```text
sample_bits = 2 * component_bits
sample_bytes = sample_bits / 8
```

Project VDIF v1 的 raw wire contract 使用以下对应关系，避免单独的 `NBIT` 产生
语义歧义：

```text
SAMPLE_FORMAT=CI8
COMPONENT_NBIT=8
SAMPLE_NBIT=16
SOURCE_COMPONENT_ORDER=IQ
COMPONENT_ORDER=RI
ENDIAN=LITTLE
```

wire header 的 `component_bits_minus_one` 对 CI8 为 7、对 CI16 为 15；有符号性由
`sample_encoding=TWOS_COMPLEMENT` 与 `CI8/CI16` 类型共同决定，不另设 `signed`。
`vdif_unpack` 将 IQ 语义映射为计算 ring 的 RI 语义，不改变两个分量的存储位置。

## 5. 模块输入输出总表

| 模块 | 输入 | 输出 | 主要变化 |
| --- | --- | --- | --- |
| `vdif_unpack` | raw records | `T-F-P-A`, `CI8/CI16/...`, host | 去包头、校验、重排 |
| `host_to_device` | `T-F-P-A`, host/pinned host | shape/type 不变，CUDA device | 只改变内存位置 |
| `complex_convert` | `T-F-P-A`, `CI8/CI16`, host 或 CUDA device | `T-F-P-A`, `CF32`，位置不变 | 转换为逻辑复数浮点 |
| `beamform` | 数据 `T-F-P-A`，系数 `F-P-A-B` | `T-F-P-B`, `CF32` | `A` 轴变为 `B` 轴 |
| `power` | `T-F-P-B`, `CF32` | `T-F-P-B`, `F32` | 复电压变成功率 |
| `stokes` | `T-F-2-B`, `CF32` | `T-F-B-S`, `F32` | 生成 4 个相关产物 |
| `time_integrate` | `T-F-P-B` 或 `T-F-B-S`, `F32` | `T/K-F-P-B` 或 `T/K-F-B-S` | 时间轴按 `K` 缩短 |
| `device_to_host` | 任意受支持的 CUDA tensor | shape/type 不变，host/pinned host | 只改变内存位置 |

## 6. `vdif_unpack`

### 输入

```text
Memory: Host 或 PinnedHost
Layout: 每条 record = 32-byte Project VDIF v1 header + TFP payload
Block: records_per_block 条完整 record
```

### 输出

```text
Memory: Host 或 PinnedHost
Order: TFPA
Type: 由 sample_format 决定，例如 CI8 或 CI16
Shape: [T, F, P, A]
```

### 职责

- 解析和校验固定 32-byte/8-word Project VDIF v1 header。
- 校验包序号、时间戳、channel、polarization 和 antenna 标识。
- 检测丢包、重复包和乱序包。
- 按配置执行零填充、报错或其他丢包策略。
- 去掉 record header。
- 按 `antenna_map[station_id]` 聚合所有天线并将 `TFP` 重排为 `TFPA`。
- 保持整数复数表示，不在该模块中转换为 `cuComplex`。

### Header 更新

```text
DATA_STAGE=UNPACKED
ORDER=TFPA
RECORD_HEADER_BYTES=0
SAMPLE_FORMAT=CI8/CI16/...
MEMORY=HOST 或 PINNED_HOST
```

`RECORD_BYTES`、`RESOLUTION` 使用一个完整 TFPA 时间帧
`F*P*A*complex_sample_bytes`；compute block bytes 等于所有输入 packet payload bytes
之和。payload/raw byte rate 按同一 packet time 内的全部 A 个天线计算。

## 7. `host_to_device`

`host_to_device` 只负责异步内存传输，不执行数值类型转换：

```text
TFPA/CI8/Host -> TFPA/CI8/CudaDevice
TFPA/CI16/Host -> TFPA/CI16/CudaDevice
```

`cudaMemcpyAsync()` 完成前，worker 不能释放输入 ring block lease。输出 metadata 只
更新：

```text
MEMORY=CUDA_DEVICE
CUDA_DEVICE=<gpu_id>
```

第一版实现要求 `RESOLUTION>0`，每个 block 必须包含整数个 `RESOLUTION`。模块从
worker 提供的 `BlockExecutionContext` 获取 non-default stream，只排队拷贝；显存申请、
stream 同步和 buffer 生命周期均由 worker 负责。

## 8. `complex_convert`

解包后的整数复数数据转换为逻辑 `CF32`；CUDA backend 的存储与 `cuComplex` 兼容，
但 PSRDADA header 不依赖 CUDA 类型名称。CPU reference 用于便携 oracle。

```text
Input:  X[T,F,P,A], CI8/CI16, host 或 CUDA device
Output: Y[T,F,P,A], CF32，memory location 与输入一致
```

示例计算：

```text
Y.real = scale * X.real
Y.imag = scale * X.imag
```

第一版仅支持以下无歧义格式：

```text
SAMPLE_FORMAT=CI8  -> COMPONENT_NBIT=8,  SAMPLE_NBIT=16
SAMPLE_FORMAT=CI16 -> COMPONENT_NBIT=16, SAMPLE_NBIT=32
COMPONENT_ORDER=RI
COMPONENT_SIGNED=1
ENDIAN=LITTLE
```

模块参数为：

```text
CONVERSION_SCALE=<positive finite scalar>
EXECUTION_BACKEND=CPU_REFERENCE 或 CUDA
CUDA_DEVICE=<gpu_id，仅 CUDA>
```

第一版计算为 `Y=CONVERSION_SCALE*X`，offset 固定为零，不自动归一化或饱和。
`CONVERSION_SCALE` 是整个 tensor 共用的反量化比例；如果后续需要 per-channel 或
per-antenna scale，应升级配置 schema。模块不改变轴顺序或 shape。

CPU reference 接受 `HOST/PINNED_HOST`，CUDA backend 要求输入输出均为
`CUDA_DEVICE`，并只向 worker 提供的 non-default stream 异步提交 kernel。两种 backend
均禁止输入输出 buffer 重叠。

Header 更新：

```text
DATA_STAGE=CONVERTED
ORDER=TFPA
SAMPLE_FORMAT=CF32
COMPONENT_NBIT=32
SAMPLE_NBIT=64
SOURCE_SAMPLE_FORMAT=CI8 或 CI16
SOURCE_COMPONENT_NBIT=8 或 16
SOURCE_COMPONENT_SIGNED=1
RECORD_BYTES=F*P*A*8
RESOLUTION=F*P*A*8
```

`BYTES_PER_SECOND` 以及可选 `TRANSFER_SIZE/FILE_SIZE/OBS_OFFSET` 按 input/output
frame bytes 比例精确缩放；未知观测 metadata 保留。

为性能优化，可以将 H2D 和 conversion 在实现层融合，但逻辑上仍保留两个模块
契约、两个 header transform 和独立测试。

## 9. `beamform`

### 输入和系数

```text
X: [T,F,P,A], CF32, CUDA device
W: [F,P,A,B], CF32 或明确声明的受支持类型, CUDA device
```

系数不随 `T` 变化，在一个 transfer 内固定。worker 在启动或读取新 transfer header
后加载一次系数并常驻 GPU；每个数据 block 不重复加载。

### 第一版权重文件

第一版使用 NumPy `.npy` 文件保存量化前的复权重分量。文件必须满足：

```text
shape: [F,P,A,B,2]
order: FPAB2, C-contiguous
component 0: real
component 1: imag
dtype: |i1 (CI8) 或 <i2 (little-endian CI16)
```

不接受 Fortran order、压缩 `.npz`、大端 int16、隐式转置或带尾随字节的文件。
文件的 `F/P/A/B` 必须分别等于输入 header/config 中的
`NCHAN/NPOL/NANT/NBEAM`。

整数权重通过显式配置的全局反量化比例转换为 CF32：

```text
W.real = raw_real * WEIGHTS_SCALE
W.imag = raw_imag * WEIGHTS_SCALE
```

`WEIGHTS_SCALE` 必须是有限正数，不能只根据 component bit width 自动猜测。
阵元整数数据和权重可以使用相同 component bit width，但二者的 scale 是不同的
物理量。后续如需按 channel/antenna 使用不同 scale，应升级权重文件 schema，不能
改变全局 `WEIGHTS_SCALE` 的含义。

第一版模块参数为：

```text
WEIGHTS_FILE=<path/to/weights.npy>
WEIGHTS_ORDER=FPAB2
WEIGHTS_ID=<version/hash/observation identity>
WEIGHTS_SCALE=<positive finite scalar>
NBEAM=<B>
EXECUTION_BACKEND=CPU_REFERENCE 或 CUDA
CUDA_DEVICE=<gpu_id，仅 CUDA>
COMPUTE_MODE=FP32 或 TF32
```

macOS 上的 portable reference backend 只执行 host/pinned-host FP32，用于验证
布局、反量化和数值结果。CUDA backend 将权重转换为 FPAB `cuComplex` 后，在一个
transfer 内常驻 `CUDA_DEVICE` 显存；数据 block 必须使用 `kCudaDevice` input/output。

CUDA FP32 使用 `CUBLAS_COMPUTE_32F`。CUDA TF32 使用
`CUBLAS_COMPUTE_32F_FAST_TF32`，启动时要求 GPU compute capability 至少为 8.0。
不支持 TF32 的设备或非 CUDA build 必须明确报错，不能静默退化为 FP32。

### 计算

对每个 `(f,p)` 执行批量矩阵乘：

```text
X[f,p]: [T,A]
W[f,p]: [A,B]
Y[f,p] = X[f,p] * W[f,p]: [T,B]
```

整体变换：

```text
[T,F,P,A] x [F,P,A,B] -> [T,F,P,B]
batch_count = F * P
```

### 输出

```text
Y: [T,F,P,B], CF32, CUDA device
ORDER=TFPB
```

### Header 更新

```text
DATA_STAGE=BEAMFORMED
ORDER=TFPB
SAMPLE_FORMAT=CF32
NBEAM=<B>
WEIGHT_ORDER=FPAB
WEIGHTS_ID=<version/path/hash>
WEIGHTS_INPUT_ORDER=FPAB2
WEIGHTS_INPUT_DTYPE=|i1 或 <i2
WEIGHTS_SCALE=<反量化比例>
COMPUTE_MODE=FP32 或 TF32
EXECUTION_BACKEND=CPU_REFERENCE 或 CUDA
CUDA_DEVICE=<gpu_id，仅 CUDA>
```

`NANT` 保留为观测来源信息，不能用 beam 数覆盖它。

## 10. `power`

### 计算

```text
Input:  V[T,F,P,B], CF32, DATA_STAGE=BEAMFORMED
Output: Q[T,F,P,B], F32

Q[t,f,p,b] = V.real^2 + V.imag^2
```

`power` 不执行时间积分，所以 `T`、`TSAMP` 和轴顺序保持不变。它只改变数据类型、
数据语义、frame bytes、block bytes 和 byte rate。

```text
DATA_STAGE=POWER
ORDER=TFPB
SAMPLE_FORMAT=F32
PRODUCTS=POWER
```

Power 已丢失复数相位，不能作为 `beamform` 或 `stokes` 的输入。

第一版参数为：

```text
EXECUTION_BACKEND=CPU_REFERENCE 或 CUDA
CUDA_DEVICE=<gpu_id，仅 CUDA>
```

Power 是逐元素 FP32 运算，不使用 `COMPUTE_MODE`。CUDA 后端要求输入、输出均在
`CUDA_DEVICE`，并在 worker 提供的 non-blocking stream 上异步启动 kernel；模块
不能在 `ProcessBlock()` 内执行 stream 或 device 同步。输入和输出 buffer 不允许
重叠，因为并行的 CF32→F32 压缩写入会覆盖尚未读取的复数样本。

## 11. `stokes`

### 输入约束

```text
Input: V[T,F,P,B], CF32, DATA_STAGE=BEAMFORMED
P必须等于2
```

代码中将两路复数偏振记为：

```text
X = V[t,f,0,b]
Y = V[t,f,1,b]
```

### 输出相关产物

```text
AA      = X * conj(X) = |X|^2
BB      = Y * conj(Y) = |Y|^2
AB_REAL = Re(X * conj(Y))
AB_IMAG = Im(X * conj(Y))
```

```text
Output: C[T,F,B,S], F32
S = [AA, BB, AB_REAL, AB_IMAG]
ORDER=TFBS
```

这些量严格来说是 coherency/correlation products。它们可以进一步转换为
`I/Q/U/V`：

```text
I = AA + BB
Q = AA - BB
U = 2 * AB_REAL
V = sign * 2 * AB_IMAG
```

第一版 `stokes` 模块输出相关产物，不隐式转换成 `I/Q/U/V`。header 必须明确：

```text
DATA_STAGE=POLARIZATION_PRODUCTS
ORDER=TFBS
SAMPLE_FORMAT=F32
NPOL=2
NPRODUCT=4
PRODUCTS=AA,BB,AB_REAL,AB_IMAG
POL_LABELS=<实际偏振标签>
```

`POL_LABELS` 是必填字段，由上游明确两路偏振的物理含义并原样传到输出。第一版
参数为：

```text
EXECUTION_BACKEND=CPU_REFERENCE 或 CUDA
CUDA_DEVICE=<gpu_id，仅 CUDA>
```

Stokes 是逐元素 FP32 运算，不使用 `COMPUTE_MODE`。CUDA 后端要求输入、输出均在
`CUDA_DEVICE`，并在 worker stream 上异步启动 kernel。输入和输出 buffer 不允许
重叠。两个 CF32 输入生成四个 F32，因此一个时间帧的字节数和
`BYTES_PER_SECOND` 保持不变。

## 12. `time_integrate`

积分是独立模块，不包含在 `power` 或 `stokes` 内。

### 第一版输入约束

- 输入必须是 `F32` 实数 tensor。
- 支持 `TFPB` 和 `TFBS`。
- 配置积分长度 `K=integration_length`，且 `K > 0`。
- 每个输入 block 的 `T` 必须满足 `T % K == 0`。
- 不跨 block 保存未完成积分，不在 EOD 时拼接相邻 block。
- CPU 和 CUDA 第一版均使用 FP32 累加；`mean` 在累加后乘 `1/K`。
- 输入必须提供正数 `BYTES_PER_SECOND` 和正数有限值 `TSAMP`。
- `RECORD_BYTES`、`RESOLUTION` 必须等于一个完整 F32 时间帧。

### 输入输出

```text
TFPB: [T,F,P,B] -> [T/K,F,P,B]
TFBS: [T,F,B,S] -> [T/K,F,B,S]
```

每个输入 block 仍对应一个输出 block，但输出 block 的有效字节数和 ring capacity
按积分长度缩小：

```text
T_out = T_in / K
output_block_bytes = input_block_bytes / K
```

支持的积分运算通过配置明确：

```json
{
  "type": "time_integrate",
  "parameters": {
    "length": 128,
    "operation": "mean"
  }
}
```

`operation` 第一版允许 `sum` 或 `mean`。Header 更新：

```text
DATA_STAGE=<上游阶段>_INTEGRATED
INTEGRATION_LENGTH=K
INTEGRATION_OPERATION=SUM 或 MEAN
TSAMP=上游 TSAMP * K
```

若上游已经做过积分，累计积分长度相乘：

```text
TOTAL_INTEGRATION_LENGTH = upstream_total * K
```

输出 `BYTES_PER_SECOND` 按 `1/K` 缩小；存在时，`TRANSFER_SIZE`、`FILE_SIZE` 和
`OBS_OFFSET` 也按 `1/K` 缩小，所有除法必须整除。输出 `RESOLUTION` 和
`RECORD_BYTES` 使用一个完整输出时间帧的字节数。输入输出 buffer 不允许重叠；非整除
block 在第一版中直接报错，不能静默丢弃或补齐。

## 13. `device_to_host`

`device_to_host` 只传输内存，不改变数据类型、shape 或 order：

```text
CudaDevice -> Host 或 PinnedHost
```

`cudaMemcpyAsync()` 完成前，worker 不能提交输出 ring block。输出 metadata 更新：

```text
MEMORY=HOST 或 PINNED_HOST
```

目标类型由模块参数 `OUTPUT_MEMORY` 指定。当前 PSRDADA output ring 使用 `HOST`；后续
若 worker 对共享内存 block 完成 `cudaHostRegister()`，或使用 pinned bounce buffer，才
声明为 `PINNED_HOST`。模块只排队 D2H，不负责注册共享内存，也不提交 ring block。

## 14. 合法和非法模块链

### 解包

```text
vdif_unpack
```

### 波束复电压

```text
host_to_device -> complex_convert -> beamform -> device_to_host
```

### 波束功率和积分

```text
host_to_device -> complex_convert -> beamform -> power
    -> time_integrate -> device_to_host
```

### 偏振相关产物和积分

```text
host_to_device -> complex_convert -> beamform -> stokes
    -> time_integrate -> device_to_host
```

### 非法链

```text
power -> beamform
power -> stokes
time_integrate(CF32 input)
stokes(NPOL != 2)
beamform(input order != TFPA)
```

`power` 和 `stokes` 都需要 beamformed complex voltage。如果同时需要独立 power
和 polarization-product 输出，应将 beamformed ring 配置为两个 reader，并运行两个
worker；第一版 worker 不支持两个输出 ring。由于 `stokes` 输出已经包含 `AA/BB`，
多数场景不需要再单独运行 `power`。

## 15. `pipeline_worker` 调用规则

### 启动阶段

1. 读取 worker JSON。
2. 连接一个 input HDU 和一个 output HDU。
3. 根据 `output.product` 创建 `beamform`、`beamform+power` 或
   `beamform+stokes` 模块链；若启用积分，在 Power/Stokes 后追加
   `time_integrate`。
4. 读取并解析输入 header，得到 `Metadata H0` 和输入 `TensorSpec`。
5. 依次调用模块的配置/规划接口，得到 `H1 ... Hout` 和每级 TensorSpec。
6. 验证完整模块链、CUDA device、权重维度和每级 block bytes。
7. 验证 output ring data block capacity 等于最终计划值。
8. 发布最终输出 header。
9. 开始读取 data block。

### 每个 block

```text
input ring BlockLease
  -> TensorView
  -> module 0
  -> intermediate buffer 0/1
  -> module 1
  -> ...
  -> final output buffer
  -> output ring commit
  -> input ring release
```

每个模块返回本次唯一输出 block 的有效字节数。worker 使用 host/device buffer pool
管理中间 block，不让模块直接持有 ring block。涉及异步 CUDA 操作时，ring lease 和
buffer 只能在对应 CUDA event 完成后释放或提交。

worker 通过 `BlockExecutionContext` 向每个模块传入非持有的 CUDA device 和 stream。
CUDA 模块的 `ProcessBlock()` 只向该 stream 提交工作并返回，不在模块内部执行逐
block `cudaDeviceSynchronize()` 或 `cudaStreamSynchronize()`。

当前正确性优先版本在一个 transfer 内使用一条 non-blocking stream。worker 依次提交
H2D、Beamform、Power/Stokes、D2H，然后在提交 output ring block 和释放 input ring
block 前调用 `cudaStreamSynchronize()`。这样已经保证 ring lease 和 device buffer 的
生命周期正确，但还没有跨 block overlap。

后续性能版本使用多个 stream/buffer slot 和 CUDA event：

典型双 buffer 流水为：

```text
stream 0: block N   H2D -> convert -> beamform -> power/stokes -> D2H
stream 1: block N+1 H2D -> convert -> beamform -> power/stokes -> D2H
```

性能版本必须在每条链末尾记录 event。event 完成后才能提交输出 ring block、释放
input ring lease 并复用该 buffer slot。worker 必须先完成所有 stream/event，再调用
模块 `Finish()` 和销毁 stream。

### EOD

模块不允许跨 block 残留不完整时间帧或不完整积分，因此 `Finish()` 只负责：

- 同步未完成的 CUDA event。
- 检查模块内部无残留数据。
- 释放 observation 级资源。
- 向输出 ring 传播 EOD。

## 16. Block 几何

单时间帧字节数：

```text
TFPA = F * P * A * sizeof(input_complex)
TFPB complex = F * P * B * sizeof(CF32)
TFPB power = F * P * B * sizeof(F32)
TFBS products = F * B * 4 * sizeof(F32)
```

Block 字节数：

```text
T = samples_per_udp * udp_packets_per_antenna_per_block
input_block_bytes = T * TFPA_frame_bytes
udp_antenna_group_bytes = udp_payload_bytes * A
input_block_bytes % udp_antenna_group_bytes = 0
product_block_bytes = T * selected_output_frame_bytes
output_block_bytes = T * selected_output_frame_bytes
```

`udp_packets_per_antenna_per_block` 是每个阵元的包数；一个完整 UDP 时间分组包含
`A` 个包。配置中的 F/A/P 与 input header 的 `NCHAN/NANT/NPOL` 必须一致。这里
`F×A×P×T` 表示维度大小，实际线性内存顺序仍由 `ORDER=TFPA` 定义。

经过长度为 `K` 的积分后：

```text
output_T = input_T / K
output_block_bytes = output_T * output_frame_bytes
```

`pipelinectl` 根据最终模块链计算每个 ring 的 data block size；`pipeline_worker` 在
运行时再次校验，不能依赖未验证的 `dada_db -b` 参数。

## 17. 配置示例

```json
{
  "schema_version": 2,
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
  },
  "integration": {
    "enabled": true,
    "length": 128,
    "operation": "mean"
  }
}
```

这是已实现的 schema v2。它固定 Beamform 为首个算法，用 `output.product` 选择
无后处理、Power 或 Stokes，并允许在 Power/Stokes 后追加积分。schema v1 仍以
“积分关闭、K=1”兼容读取。支持任意模块数组和 GPU ring 的通用 schema 需要在相应
模块实现后再次升级 schema version，不能悄悄改变 v2 语义。

## 18. 开发和测试要求

每个模块按以下顺序开发和确认：

1. 定义输入/输出 TensorSpec 和 header transform。
2. 编写启动阶段的非法组合测试。
3. 编写 CPU reference 或小规模确定性测试向量。
4. 实现模块数据路径。
5. 在 macOS 运行不依赖 CUDA/PSRDADA 的 contract 和 reference tests。
6. 在 Linux/CUDA 服务器运行 kernel correctness、block geometry 和 EOD 测试。
7. 对比 CPU reference 与 GPU 输出后，再开始下一个模块。

后续实现顺序、每阶段完成标准、统一的输入输出门禁和 JSON 参数扩展规则见
[`DEVELOPMENT_PLAN.md`](DEVELOPMENT_PLAN.md)。该计划是当前执行基线；本文件只定义
数据和模块契约，不再维护一份可能与实施进度不一致的开发顺序。

## 19. 实现前仍需确认的信息

固定 32-byte Project VDIF v1 header、TFP/IQ payload、Two's Complement 编码和
Station-ID→A 聚合规则已确定，详见
[`PROJECT_VDIF_PROFILE_V1.md`](PROJECT_VDIF_PROFILE_V1.md)。后续还需要：

- FPGA 生成的 binary golden records 及逐字段、逐 sample expected 值。
- raw integer 转 `CF32` 的 scale、offset 和饱和规则。
- 丢包、重复、乱序、invalid-data 和不完整天线 group 的处理策略。
- 两路偏振的实际标签，以及 `AB_IMAG` 的符号约定。
- 每个 ring 的目标 `T`、block 数量和 CUDA device。
