# Applications

Final executables live here. Each processing application remains a thin
composition root: load configuration, construct an algorithm module chain, and
run it between PSRDADA HDUs.

Application directories:

- `rdma2dada`: implemented NIC to raw host ring A entry point.
- `fpga_sender_sim`: implemented deterministic UDP/Project VDIF v1 source;
  each process simulates one Station ID and all instances may target the same
  receiver IP/port.
- `vdif_unpack_worker`: implemented CPU/PSRDADA raw-to-compute worker; validates
  each transfer header, reorders Project VDIF TFP packets into payload-only
  TFPA blocks, zero-fills missing Stations and propagates EOD.
- `pipeline_worker`: implemented first-version composition of Beamform followed
  by optional Power or Stokes, with one input ring and one output ring.
- `dada2rdma`: planned ring D to repacketized network output.
- `pipelinectl`: planned ring/topology validation and process supervision.

The first worker accepts `CONVERTED/TFPA/CF32` host-ring input. H2D and D2H are
currently process-owned transfers around the selected CUDA module chain.
Integer-to-CF32 conversion and integration are implemented modules, but only
integration is currently connected to the fixed GPU worker chains. Complex
Convert worker integration and the general module registry remain planned work,
not permanent executable boundaries.

`fpga_sender_sim` 不连接 PSRDADA ring；它位于最前端，模拟 FPGA/NIC 输入。BURST 模式
用于本地功能测试，REALTIME 模式让多台服务器等待共同未来 UTC 后按整数皮秒时间轴发送。

`dada_dbdisk` remains an optional external consumer attached to any configured
host ring. Ring reader counts are derived from the enabled sinks and workers.
