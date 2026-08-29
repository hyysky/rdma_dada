# Applications

Final executables live here. Each processing application remains a thin
composition root: load configuration, construct an algorithm module chain, and
run it between PSRDADA HDUs.

Application directories:

- `rdma2dada`: implemented direct NIC to raw host ring A entry point; one
  QP/CQ/thread uses NSGE=2 to place the VDIF record directly in the PSRDADA
  slot while the network header lands in scratch memory.
- `fpga_sender_sim`: implemented deterministic UDP/Project VDIF v1 source.
  Sender schema v2 assigns one Station ID to a process; schema v3 assigns a
  Station shard to one process/socket per sender host and interleaves the
  configured Station IDs on that socket. The production A=469 profile uses two
  sender processes with 235/234 Stations and a shared receiver endpoint.
- `vdif_unpack_worker`: implemented parallel CPU/PSRDADA raw-to-compute worker;
  a coordinator dispatches pointer-free descriptors to fixed parser workers and
  a sole writer publishes ordered payload-only ATFP blocks, zero-fills missing
  Stations and propagates EOD.
- `pipeline_worker`: implemented Resolved-Plan-driven composition of fused
  ATFP/CI8-to-TFPA/CF32 conversion, Beamform, optional Power or Stokes, optional
  Time Integration, and D2H between one compute ring and one output ring.
- `dada2rdma`: planned ring D to repacketized network output.
- `pipelinectl`: planned ring/topology validation and process supervision.

The GPU worker accepts `UNPACKED/ATFP/CI8` host-ring input. H2D and D2H are
process-owned transfers around the selected CUDA module chain. Complex Convert
performs the physical ATFP-to-TFPA transpose, integer-to-CF32 conversion and
explicit scale application on the worker-owned stream. The current product
chains are fixed and compatibility-checked; the general module registry remains
planned work, not a permanent executable boundary. Production A=469 Full
Power and coherency-product chains have both completed the nominal 30 Gb/s,
60 s, warm-up plus three-measured-run acceptance campaign.

`fpga_sender_sim` 不连接 PSRDADA ring；它位于最前端，模拟 FPGA/NIC 输入。BURST 模式
用于本地功能测试，REALTIME 模式让多台服务器等待共同未来 UTC 后按整数皮秒时间轴发送。

`dada_dbdisk` remains an optional external consumer attached to any configured
host ring. Ring reader counts are derived from the enabled sinks and workers.

功能实现、服务器验收、性能验收和后续顺序的统一状态见
[`docs/PROJECT_STATUS.md`](../docs/PROJECT_STATUS.md)。
