# Stokes / polarization-products module

Stokes 是波束合成之后与 Power 并列的可选算法模块，不能消费 Power 输出。第一版
只接受 `DATA_STAGE=BEAMFORMED`、`TFPB/CF32` 且 `NPOL=2` 的波束复电压。

对于每个 `[t,f,b]`，两路偏振记为 `X` 和 `Y`，模块计算：

```text
AA      = |X|^2
BB      = |Y|^2
AB_REAL = Re(X * conj(Y))
AB_IMAG = Im(X * conj(Y))
```

输出为 `TFBS/F32`，`S` 的顺序固定为：

```text
[AA, BB, AB_REAL, AB_IMAG]
```

当前工程和论文验证范围固定为这四个 coherency/correlation products，不包含
`I/Q/U/V` 的物理输出或推导。

## 参数

```text
EXECUTION_BACKEND=CPU_REFERENCE
```

或：

```text
EXECUTION_BACKEND=CUDA
CUDA_DEVICE=0
```

Stokes 是逐元素 FP32 运算，不使用 `COMPUTE_MODE` 或 Tensor Core。CUDA kernel 在
worker 提供的 non-blocking stream 上异步执行，不在 `ProcessBlock()` 中同步。

## Header 更新

```text
DATA_STAGE=POLARIZATION_PRODUCTS
ORDER=TFBS
SAMPLE_FORMAT=F32
NPOL=2
NPRODUCT=4
PRODUCTS=AA,BB,AB_REAL,AB_IMAG
COMPONENT_NBIT=32
SAMPLE_NBIT=32
RECORD_BYTES=F*B*4*sizeof(float)
RESOLUTION=F*B*4*sizeof(float)
```

由于两个 CF32 输入恰好对应四个 F32 输出，block 字节数和
`BYTES_PER_SECOND` 不变。`POL_LABELS` 必须由上游以 `<P0>,<P1>` 格式提供并
原样保留。输入和输出 buffer 不允许重叠，worker 必须分配独立输出区。
