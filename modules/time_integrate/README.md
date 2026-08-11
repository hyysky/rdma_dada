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

## CPU performance benchmark

Build the optional benchmark in Release mode and pass `T`, frame element count,
integration length, iteration count and operation:

```bash
cmake -S . -B build-perf \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_RDMA_PIPELINE=OFF \
  -DBUILD_ALGORITHM_BENCHMARKS=ON
cmake --build build-perf --target time_integrate_benchmark --parallel
./build-perf/time_integrate_benchmark 65536 180 128 40 MEAN
```

The tool runs through the public module API, checks deterministic output and
reports effective input GB/s. Performance comparisons must use the same build,
dimensions, operation and machine.

The loop-order optimization was measured on 2026-08-05 with an AppleClang 21
Release build. These numbers are a local A/B record, not a server acceptance
threshold:

| `frame_elements` | `K` | previous | contiguous-frame loop |
| ---: | ---: | ---: | ---: |
| 180 | 128 | 7.2 GB/s | 51.9 GB/s |
| 8 | 128 | 11.4 GB/s | 33.2 GB/s |
| 360 | 1024 | 6.6 GB/s | 67.5 GB/s |

For every element, accumulation still visits time samples in the same order;
the optimization changes traversal across independent frame elements so the
CPU reads contiguous memory and can vectorize the inner loop.

### CUDA kernel benchmark

With `USE_CUDA=ON`, the same option also builds a CUDA-event benchmark. H2D and
D2H are deliberately outside the timed region, and the downloaded result is
checked against a CPU oracle:

```bash
cmake --build build-gpu --target time_integrate_cuda_benchmark --parallel
./build-gpu/time_integrate_cuda_benchmark 65536 180 128 200 MEAN 0
```

Run the actual observation geometries and several `K` values before replacing
the direct kernel with a cooperative tree reduction. Record `us_per_block` and
`input_GBps` together with the GPU model, clocks and CUDA version.
