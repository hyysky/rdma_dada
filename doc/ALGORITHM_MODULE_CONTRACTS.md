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

前端通常输出固定宽度的复数整数，例如 `8+8 bit`，即实部 8 bit、虚部 8 bit。
实际 component 位宽尚未最终确定，因此程序不能将 `CI8`、`CI16` 或 `cuComplex`
写死。

JSON 必须无歧义地描述 raw sample：

```json
{
  "sample_format": {
    "kind": "complex_integer",
    "component_bits": 8,
    "component_order": "RI",
    "signed": true,
    "endianness": "little"
  }
}
```

派生关系：

```text
sample_bits = 2 * component_bits
sample_bytes = sample_bits / 8
```

建议使用以下 header 字段，避免单独的 `NBIT` 产生语义歧义：

```text
SAMPLE_FORMAT=CI8
COMPONENT_NBIT=8
SAMPLE_NBIT=16
COMPONENT_ORDER=RI
ENDIAN=LITTLE
```

现有 contract v1 将 `NBIT=16` 固定在 header codec 中。正式支持可变位宽时，必须
先明确旧 `NBIT/PKT_NBIT` 是“单个分量位宽”还是“完整复数 sample 位宽”，再升级
contract version；不能仅删除校验而保留含糊语义。

## 5. 模块输入输出总表

| 模块 | 输入 | 输出 | 主要变化 |
| --- | --- | --- | --- |
| `vdif_unpack` | raw records | `T-F-P-A`, `CI8/CI16/...`, host | 去包头、校验、重排 |
| `host_to_device` | `T-F-P-A`, host/pinned host | shape/type 不变，CUDA device | 只改变内存位置 |
| `complex_convert` | `T-F-P-A`, `CI8/CI16/...`, CUDA device | `T-F-P-A`, `CF32`, CUDA device | 转换为 `cuComplex` |
| `beamform` | 数据 `T-F-P-A`，系数 `F-P-A-B` | `T-F-P-B`, `CF32` | `A` 轴变为 `B` 轴 |
| `power` | `T-F-P-B`, `CF32` | `T-F-P-B`, `F32` | 复电压变成功率 |
| `stokes` | `T-F-2-B`, `CF32` | `T-F-B-S`, `F32` | 生成 4 个相关产物 |
| `time_integrate` | `T-F-P-B` 或 `T-F-B-S`, `F32` | `T/K-F-P-B` 或 `T/K-F-B-S` | 时间轴按 `K` 缩短 |
| `device_to_host` | 任意受支持的 CUDA tensor | shape/type 不变，pinned host | 只改变内存位置 |

## 6. `vdif_unpack`

### 输入

```text
Memory: Host 或 PinnedHost
Layout: 每条 record = 64-byte application header + payload
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

- 解析和校验 64-byte application header。
- 校验包序号、时间戳、channel、polarization 和 antenna 标识。
- 检测丢包、重复包和乱序包。
- 按配置执行零填充、报错或其他丢包策略。
- 去掉 record header。
- 将前端 payload order 重排为 `TFPA`。
- 保持整数复数表示，不在该模块中转换为 `cuComplex`。

### Header 更新

```text
DATA_STAGE=UNPACKED
ORDER=TFPA
RECORD_HEADER_BYTES=0
SAMPLE_FORMAT=CI8/CI16/...
MEMORY=HOST 或 PINNED_HOST
```

`RECORD_BYTES`、`RESOLUTION`、block bytes 和 byte rate 按 payload-only 数据重新计算。

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

## 8. `complex_convert`

解包后的整数复数数据在 GPU 上转换为 `cuComplex`。模块逻辑类型使用 `CF32`，
避免让 PSRDADA header 依赖 CUDA 类型名称。

```text
Input:  X[T,F,P,A], CI8/CI16/..., CUDA device
Output: Y[T,F,P,A], CF32, CUDA device
```

示例计算：

```text
Y.real = scale * X.real
Y.imag = scale * X.imag
```

`scale`、offset、饱和处理和是否归一化由模块配置决定。第一版不允许该模块改变
轴顺序或 shape。

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
Input:  V[T,F,P,B], CF32
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

## 11. `stokes`

### 输入约束

```text
Input: V[T,F,P,B], CF32, CUDA device
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

## 12. `time_integrate`

积分是独立模块，不包含在 `power` 或 `stokes` 内。

### 第一版输入约束

- 输入必须是 `F32` 实数 tensor。
- 支持 `TFPB` 和 `TFBS`。
- 配置积分长度 `K=integration_length`，且 `K > 0`。
- 每个输入 block 的 `T` 必须满足 `T % K == 0`。
- 不跨 block 保存未完成积分，不在 EOD 时拼接相邻 block。

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
TSAMP_OUT=TSAMP_IN * K
```

若上游已经做过积分，累计积分长度相乘：

```text
TOTAL_INTEGRATION_LENGTH = upstream_total * K
```

输出 `BYTES_PER_SECOND` 按 `1/K` 缩小，输出 `RESOLUTION` 使用一个完整输出时间帧
的字节数。非整除 block 在第一版中直接报错，不能静默丢弃或补齐。

## 13. `device_to_host`

`device_to_host` 只传输内存，不改变数据类型、shape 或 order：

```text
CudaDevice -> PinnedHost
```

`cudaMemcpyAsync()` 完成前，worker 不能提交输出 ring block。输出 metadata 更新：

```text
MEMORY=PINNED_HOST
```

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
3. 通过 `ModuleRegistry` 按顺序创建模块。
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
同一个 block 的 GPU 模块链使用同一 stream；不同 block 可以使用不同 stream 和
buffer slot。CUDA 模块的 `ProcessBlock()` 只向该 stream 提交工作并返回，不执行逐
block `cudaDeviceSynchronize()` 或 `cudaStreamSynchronize()`。

典型双 buffer 流水为：

```text
stream 0: block N   H2D -> convert -> beamform -> power/stokes -> D2H
stream 1: block N+1 H2D -> convert -> beamform -> power/stokes -> D2H
```

worker 在每条链末尾记录 event。event 完成后才能提交输出 ring block、释放输入 ring
lease 并复用该 buffer slot。worker 必须先完成所有 stream/event，再调用模块
`Finish()` 和销毁 stream。

### EOD

第一版模块不允许跨 block 残留不完整时间帧或不完整积分，因此 `Finish()` 只负责：

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
block_bytes = T * frame_bytes
```

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
  "name": "stokes_worker",
  "input_ring": "b000",
  "output_ring": "d000",
  "cuda_device": 0,
  "modules": [
    {
      "type": "host_to_device"
    },
    {
      "type": "complex_convert",
      "parameters": {
        "output_datatype": "CF32",
        "scale": 1.0
      }
    },
    {
      "type": "beamform",
      "parameters": {
        "weights_file": "config/beam_weights.bin",
        "weights_order": "FPAB",
        "nbeam": 90
      }
    },
    {
      "type": "stokes",
      "parameters": {
        "output_basis": "AA_BB_AB",
        "products": ["AA", "BB", "AB_REAL", "AB_IMAG"]
      }
    },
    {
      "type": "time_integrate",
      "parameters": {
        "length": 128,
        "operation": "mean"
      }
    },
    {
      "type": "device_to_host"
    }
  ]
}
```

## 18. 开发和测试要求

每个模块按以下顺序开发和确认：

1. 定义输入/输出 TensorSpec 和 header transform。
2. 编写启动阶段的非法组合测试。
3. 编写 CPU reference 或小规模确定性测试向量。
4. 实现模块数据路径。
5. 在 macOS 运行不依赖 CUDA/PSRDADA 的 contract 和 reference tests。
6. 在 Linux/CUDA 服务器运行 kernel correctness、block geometry 和 EOD 测试。
7. 对比 CPU reference 与 GPU 输出后，再开始下一个模块。

第一阶段建议实现顺序：

```text
TensorSpec/ModulePlan
  -> ModuleChain mock tests
  -> vdif_unpack
  -> pipeline_worker + mock ring
  -> PSRDADA ring adapter
  -> host_to_device/device_to_host
  -> complex_convert
  -> beamform
  -> power
  -> stokes
  -> time_integrate
```

## 19. 实现前仍需确认的信息

- 64-byte application header 每个字段的偏移、位宽和 endian。
- raw payload 中 `T/F/P/A/real/imag` 的实际排列。
- `component_bits` 的实际取值和是否有符号。
- raw integer 转 `CF32` 的 scale、offset 和饱和规则。
- beam weight 文件格式、数据类型、归一化方式和版本标识。
- 两路偏振的实际标签，以及 `AB_IMAG` 的符号约定。
- 每个 ring 的目标 `T`、block 数量和 CUDA device。
