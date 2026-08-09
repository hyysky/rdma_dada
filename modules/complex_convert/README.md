# Complex integer conversion module

`ComplexConvertModule` 将解包后的块级 `A-T-F-P` 有符号整数复数阵元数据转换为
波束合成使用的、物理连续的 `T-F-P-A/CF32`。模块不拥有 ring、显存或 CUDA
stream。

第一版支持：

```text
ATFP/CI8  -> TFPA/CF32
ATFP/CI16 -> TFPA/CF32
```

输入必须为 `LAYOUT_SCOPE=BLOCK`、little-endian、`IQ` 分量顺序和
`SAMPLE_ENCODING=TWOS_COMPLEMENT`。有符号类型由 `CI8/CI16` 推导，不使用
`COMPONENT_SIGNED`。CI8 的一个复数 sample 为 2 bytes，CI16 为 4 bytes，CF32
输出固定为 8 bytes。计算定义为：

```text
output.real = input.real * CONVERSION_SCALE
output.imag = input.imag * CONVERSION_SCALE
```

对每个实际输入 block：

```text
Q = actual_T * F * P
src(a,q) = a * Q + q
dst(q,a) = q * A + a
```

`actual_T` 从 `input_bytes/(A*F*P*input_sample_bytes)` 推导，因此 EOD 的紧凑
partial block 可直接处理；存在余数时拒绝该 block。转换和 scale 在同一次遍历中完成，
scale 只应用一次。

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

CUDA backend 使用带 padding 的 shared-memory tile，在一次 kernel 中完成物理
`[A,Q] -> [Q,A]` 转置、CI8/CI16→CF32 和 scale。kernel 只提交到 worker 提供的
non-default stream，不调用 `cudaDeviceSynchronize` 或 `cudaStreamSynchronize`；同步由
上层 worker 负责。输入和输出 buffer 不能重叠。

诊断 benchmark 将 H2D 和 kernel 时间分开报告：

```bash
./build-gpu/complex_convert_transpose_cuda_benchmark \
  CI8 256 65536 200 1.0 0
```

参数依次是格式、`A`、`Q`、迭代次数、scale 和 CUDA device。输出包含输入/输出 bytes、
单次 kernel 时间及按输入+输出字节计算的有效 GB/s；benchmark 不能替代正确性测试或
整条实时 pipeline 的吞吐验收。

## Header 变换

必填输入字段包括 `DATA_STAGE=UNPACKED`、`ORDER=ATFP`、
`LAYOUT_SCOPE=BLOCK`、`SAMPLE_FORMAT=CI8|CI16`、
`SAMPLE_ENCODING=TWOS_COMPLEMENT`、`COMPONENT_ORDER=IQ`、
`NCHAN/NPOL/NANT/BLOCK_NTIME`、`OUTPUT_BLOCK_BYTES`、`RECORD_BYTES`、
`RESOLUTION` 和正数 `BYTES_PER_SECOND`。

输出更新为：

```text
DATA_STAGE=CONVERTED
ORDER=TFPA
SOURCE_ORDER=ATFP
SAMPLE_FORMAT=CF32
COMPONENT_ORDER=RI
COMPONENT_NBIT=32
SAMPLE_NBIT=64
SOURCE_SAMPLE_FORMAT=CI8 或 CI16
SOURCE_COMPONENT_NBIT=8 或 16
RECORD_BYTES=BLOCK_NTIME*F*P*A*8
OUTPUT_BLOCK_BYTES=BLOCK_NTIME*F*P*A*8
RESOLUTION=F*P*A*8
```

输出删除 `COMPONENT_SIGNED`、`SOURCE_COMPONENT_SIGNED` 和整数输入的
`SAMPLE_ENCODING`，并用 `SOURCE_SAMPLE_ENCODING=TWOS_COMPLEMENT` 保存来源语义。
`BYTES_PER_SECOND` 和可选 `TRANSFER_SIZE` 按 time-frame 对齐，`FILE_SIZE/OBS_OFFSET`
按 nominal block 对齐；所有 byte 字段按输入/输出 sample 宽度精确缩放，未对齐或溢出
时配置失败。未知观测 metadata 原样保留。

完整 pipeline 契约见
[`doc/ALGORITHM_MODULE_CONTRACTS.md`](../../doc/ALGORITHM_MODULE_CONTRACTS.md)。
