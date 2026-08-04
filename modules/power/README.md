# Power module

Power 是波束合成之后的可选独立算法模块。它只接受
`DATA_STAGE=BEAMFORMED` 的 `TFPB/CF32` 波束复电压，逐样本计算：

```text
power = real * real + imag * imag
```

输出为相同 `T-F-P-B` 顺序的 `F32` 实数。该模块不进行时间积分；需要积分时在其后
连接独立的 `time_integrate` 模块。

## 参数

CPU 参考后端用于 macOS 开发和数值验证：

```text
EXECUTION_BACKEND=CPU_REFERENCE
```

观测服务器使用 CUDA 后端：

```text
EXECUTION_BACKEND=CUDA
CUDA_DEVICE=0
```

Power 是逐元素 FP32 运算，不使用 `COMPUTE_MODE`，也不通过 Tensor Core 计算。
CUDA 版本在 `pipeline_worker` 提供的 non-blocking stream 上异步启动 kernel，不在
`ProcessBlock()` 中同步设备。由于输入为 8-byte CF32、输出为 4-byte F32，输入和
输出 buffer 不允许重叠；worker 必须为 Power 分配独立输出区。

## Header 更新

```text
DATA_STAGE=POWER
ORDER=TFPB
SAMPLE_FORMAT=F32
PRODUCTS=POWER
COMPONENT_NBIT=32
SAMPLE_NBIT=32
RECORD_BYTES=F*P*B*sizeof(float)
RESOLUTION=F*P*B*sizeof(float)
BYTES_PER_SECOND=input_bytes_per_second/2
```

`NCHAN/NPOL/NBEAM` 和未知观测字段保持不变。输入 byte rate 存在但不能精确缩放为
整数时，配置阶段直接报错。
