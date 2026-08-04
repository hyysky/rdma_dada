# Complex integer conversion module

`ComplexConvertModule` 将解包、重排后的有符号整数复数阵元数据转换为波束合成使用的
`CF32`，不改变 `T-F-P-A` shape 和线性顺序。模块不拥有 ring、显存或 CUDA stream。

第一版支持：

```text
TFPA/CI8  -> TFPA/CF32
TFPA/CI16 -> TFPA/CF32
```

输入必须为 little-endian、`RI` 分量顺序且 `COMPONENT_SIGNED=1`。CI8 的一个复数
sample 为 2 bytes，CI16 为 4 bytes，CF32 输出固定为 8 bytes。计算定义为：

```text
output.real = input.real * CONVERSION_SCALE
output.imag = input.imag * CONVERSION_SCALE
```

`CONVERSION_SCALE` 是有限正数，由配置显式传入；第一版固定零 offset，不做自动归一化
或饱和。后续需要按 channel/antenna 使用不同 scale 时，应升级配置 schema，不能改变
当前全局 scale 的语义。

## Backend

便携 CPU oracle：

```text
EXECUTION_BACKEND=CPU_REFERENCE
MEMORY=HOST 或 PINNED_HOST
```

观测服务器 CUDA backend：

```text
EXECUTION_BACKEND=CUDA
CUDA_DEVICE=<device id>
MEMORY=CUDA_DEVICE
```

CUDA backend 只在 worker 提供的 non-default stream 上异步启动 kernel；不申请 block
buffer，也不执行 stream/device 同步。输入和输出 buffer 不能重叠。

## Header 变换

必填输入字段包括 `DATA_STAGE=UNPACKED`、`ORDER=TFPA`、`SAMPLE_FORMAT`、分量格式、
`NCHAN/NPOL/NANT`、`RECORD_BYTES`、`RESOLUTION` 和正数 `BYTES_PER_SECOND`。

输出更新为：

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

`BYTES_PER_SECOND` 以及可选的 `TRANSFER_SIZE/FILE_SIZE/OBS_OFFSET` 按 input/output
frame bytes 比例精确缩放；未对齐或溢出时配置失败。未知观测 metadata 原样保留。

完整 pipeline 契约见
[`doc/ALGORITHM_MODULE_CONTRACTS.md`](../../doc/ALGORITHM_MODULE_CONTRACTS.md)。
