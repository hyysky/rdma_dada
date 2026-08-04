# Applications

Final executables live here. Each processing application should remain a thin
composition root: load configuration, construct one algorithm `Stage`, and run
it between an input and output PSRDADA HDU.

Application directories:

- `rdma2dada`: implemented NIC to raw host ring A entry point.
- `pipeline_worker`: planned ordered composition of compatible algorithm modules
  inside one process, with one input ring and one output ring.
- `dada2rdma`: planned ring D to repacketized network output.
- `pipelinectl`: planned ring/topology validation and process supervision.

`vdif_unpack`, `beamform`, `power`, `stokes`, H2D and D2H are modules selected by
`pipeline_worker`, not permanently fixed executable boundaries. Configuration
may run one module or several modules in each worker.

`dada_dbdisk` remains an optional external consumer attached to any configured
host ring. Ring reader counts are derived from the enabled sinks and workers.
