# device_to_host

Implemented byte-preserving transfer module from caller-owned CUDA device
buffers to host or pinned-host output blocks.

- Input header: `MEMORY=CUDA_DEVICE`, matching `CUDA_DEVICE`, and a positive
  `RESOLUTION`.
- Parameters: `EXECUTION_BACKEND=CUDA`, `CUDA_DEVICE=<id>` and
  `OUTPUT_MEMORY=HOST|PINNED_HOST`.
- Output header: all scientific fields are preserved; `MEMORY` changes to the
  requested host location.
- Block execution: validates complete `RESOLUTION` frames and enqueues
  `cudaMemcpyAsync(..., cudaMemcpyDeviceToHost, worker_stream)`.

The current PSRDADA worker uses `OUTPUT_MEMORY=HOST` and synchronizes the stream
before committing the output ring block. The module owns no buffer or stream.
