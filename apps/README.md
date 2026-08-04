# Applications

Final executables live here. Each processing application remains a thin
composition root: load configuration, construct an algorithm module chain, and
run it between PSRDADA HDUs.

Application directories:

- `rdma2dada`: implemented NIC to raw host ring A entry point.
- `pipeline_worker`: implemented first-version composition of Beamform followed
  by optional Power or Stokes, with one input ring and one output ring.
- `dada2rdma`: planned ring D to repacketized network output.
- `pipelinectl`: planned ring/topology validation and process supervision.

The first worker accepts `CONVERTED/TFPA/CF32` host-ring input. H2D and D2H are
currently process-owned transfers around the selected CUDA module chain.
`vdif_unpack`, integer-to-CF32 conversion, integration and the general module
registry remain planned modules, not permanent executable boundaries.

`dada_dbdisk` remains an optional external consumer attached to any configured
host ring. Ring reader counts are derived from the enabled sinks and workers.
