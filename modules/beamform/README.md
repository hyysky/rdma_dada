# beamform

Independent beamforming module with a portable FP32 reference backend and an
optional asynchronous CUDA FP32/TF32 backend.

The implemented first slice:

- reads C-contiguous NumPy `.npy` weights shaped `[F,P,A,B,2]`;
- accepts signed int8 (`|i1`) and little-endian signed int16 (`<i2`);
- dequantizes real/imaginary components with an explicit `WEIGHTS_SCALE`;
- validates weight dimensions against `NCHAN/NPOL/NANT/NBEAM`;
- transforms `TFPA/CF32` blocks to `TFPB/CF32` blocks;
- preserves source `NANT` while publishing `NBEAM` and output frame geometry;
- provides host/pinned-host FP32 processing for macOS development and reference
  comparison.
- uploads converted FPAB complex weights once per transfer when CUDA is selected;
- accepts CUDA-device TFPA/TFPB blocks and enqueues strided-batched complex GEMM
  on the worker-owned stream;
- maintains a separate cuBLAS handle for each worker stream so blocks on
  different streams can overlap;
- supports strict FP32 and TF32 compute modes without silent fallback.

The CUDA FP32/TF32 correctness tests have passed on the target server. Combined
worker/ring lifecycle and performance tests are still required. The macOS build
never includes CUDA headers or libraries.

Public API:

```text
include/rdma_dada/modules/beamform/beamform_module.h
```

Implementation and tests:

```text
modules/beamform/beamform_module.cpp
modules/beamform/beamform_cuda_backend.cu
tests/beamform_module_test.cpp
tests/beamform_cuda_test.cpp
```

The complete file/header contract is in
[`doc/ALGORITHM_MODULE_CONTRACTS.md`](../../doc/ALGORITHM_MODULE_CONTRACTS.md).
