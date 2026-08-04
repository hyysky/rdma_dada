# Time integration module

`TimeIntegrateModule` performs block-local reduction along the leading time
axis. It has no ring, buffer or CUDA-stream ownership.

Supported transforms:

```text
POWER[/_INTEGRATED]                 TFPB/F32 -> (T/K)FPB/F32
POLARIZATION_PRODUCTS[/_INTEGRATED] TFBS/F32 -> (T/K)FBS/F32
operation = SUM or MEAN
```

`K=INTEGRATION_LENGTH` must be positive and divide every input block's `T`.
The first version never carries a partial integration window across block or
EOD boundaries. CPU and CUDA both accumulate in FP32; CUDA queues its kernel on
the non-default stream supplied by the worker and does not synchronize it.

Required input header fields include the stage/order/type/shape fields,
`MEMORY`, `RECORD_BYTES`, `RESOLUTION`, positive `BYTES_PER_SECOND` and positive
finite `TSAMP`. One frame is `F*P*B*sizeof(float)` for TFPB or
`F*B*4*sizeof(float)` for TFBS. Input and output buffers must not overlap.

The module preserves unknown metadata and updates:

```text
DATA_STAGE                 = POWER_INTEGRATED or POLARIZATION_PRODUCTS_INTEGRATED
INTEGRATION_LENGTH         = K
TOTAL_INTEGRATION_LENGTH   = upstream_total * K
INTEGRATION_OPERATION      = SUM or MEAN
TSAMP                      = input TSAMP * K
BYTES_PER_SECOND           = input value / K
TRANSFER_SIZE/FILE_SIZE/OBS_OFFSET = input value / K, when present
```

All byte-count divisions must be exact. `RECORD_BYTES` and `RESOLUTION` remain
the size of one output time frame; a block and its output ring capacity shrink
by `K`.

The complete pipeline contract is documented in
[`doc/ALGORITHM_MODULE_CONTRACTS.md`](../../doc/ALGORITHM_MODULE_CONTRACTS.md).
