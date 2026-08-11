# Algorithm modules

Each subdirectory in this tree implements one `rdma_dada::pipeline::Stage`.
Algorithm modules do not own PSRDADA locks and do not start detached threads.
They transform an input header plus stage configuration into an output header,
then process data one ring block at a time.

Reference implementations are available in the sibling `DSAbeamformer` and
`phase-field-telescope` repositories. They are references rather than vendored
build dependencies.

The first module set is `vdif_unpack`, `host_to_device`, `complex_convert`,
`beamform`, `power`, `stokes`, `time_integrate`, and `device_to_host`, plus RDMA
packetization adapters. Beamforming, Power, Stokes and time integration are
separate modules. A configured worker may compose them in one process so
intermediate values remain in GPU buffers, or place ring boundaries between
them when process isolation is required.

`complex_convert` has portable and CUDA backends for signed little-endian
CI8/CI16 to CF32 conversion with an explicit scale. `beamform` has a portable
FP32 reference implementation, strict NPY weight loader and an asynchronous
CUDA FP32/TF32 backend. `power`, `stokes` and `time_integrate` have portable
FP32 reference implementations and asynchronous CUDA elementwise/reduction
backends. `host_to_device` and `device_to_host` are byte-preserving asynchronous
CUDA transfer modules and are used by `pipeline_worker`. Their standalone CUDA
correctness tests have passed on the target server. The GPU worker now runs
H2D→ATFP transpose/conversion→Beamform→Power/Stokes→optional TimeIntegrate→D2H.
Beamform-only, Power and Stokes numerical products plus the Resolved
Plan/PSRDADA/CUDA ring integration have passed server acceptance. Sustained
full-pipeline throughput and operating headroom remain under test; isolated
correctness does not establish real-time capability. Future algorithms should
use the worker-owned execution stream by default.

The authoritative data layouts, module inputs/outputs, integration rules and
worker invocation contract are documented in
[`doc/ALGORITHM_MODULE_CONTRACTS.md`](../doc/ALGORITHM_MODULE_CONTRACTS.md).

Module order is configuration-driven and compatibility-checked. Each module
must declare/validate its accepted input header and publish its output datatype,
dimensions, ordering, memory location and block geometry.

The current worker still selects from fixed legal chains. A general module
registry for constructing arbitrary compatible chains is planned but not yet
implemented. Per-module and application status is tracked in
[`docs/PROJECT_STATUS.md`](../docs/PROJECT_STATUS.md).
