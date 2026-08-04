# host_to_device

Implemented byte-preserving transfer module from host or pinned-host blocks to
caller-owned CUDA device buffers.

- Input header: `MEMORY=HOST` or `MEMORY=PINNED_HOST`, plus a positive
  `RESOLUTION`.
- Parameters: `EXECUTION_BACKEND=CUDA` and `CUDA_DEVICE=<id>`.
- Output header: all scientific fields are preserved; only
  `MEMORY=CUDA_DEVICE` and `CUDA_DEVICE=<id>` are published.
- Block execution: validates complete `RESOLUTION` frames and enqueues
  `cudaMemcpyAsync(..., cudaMemcpyHostToDevice, worker_stream)`.

The module never allocates device memory and never synchronizes or destroys the
stream. `pipeline_worker` owns both resources and keeps the input PSRDADA block
valid until the stream completes.
